#include "triton/Analysis/Utility.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include <fstream>

namespace mlir {

namespace {

class FixupLoop : public mlir::RewritePattern {

public:
  explicit FixupLoop(mlir::MLIRContext *context)
      : mlir::RewritePattern(scf::ForOp::getOperationName(), 2, context) {}

  mlir::LogicalResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    auto forOp = cast<scf::ForOp>(op);

    // Rewrite init argument
    SmallVector<Value, 4> newInitArgs = forOp.getInitArgs();
    bool shouldRematerialize = false;
    for (size_t i = 0; i < newInitArgs.size(); i++) {
      if (newInitArgs[i].getType() != forOp.getRegionIterArgs()[i].getType() ||
          newInitArgs[i].getType() != forOp.getResultTypes()[i]) {
        shouldRematerialize = true;
        break;
      }
    }
    if (!shouldRematerialize)
      return failure();

    scf::ForOp newForOp = rewriter.create<scf::ForOp>(
        forOp.getLoc(), forOp.getLowerBound(), forOp.getUpperBound(),
        forOp.getStep(), newInitArgs);
    newForOp->moveBefore(forOp);
    rewriter.setInsertionPointToStart(newForOp.getBody());
    IRMapping mapping;
    for (const auto &arg : llvm::enumerate(forOp.getRegionIterArgs()))
      mapping.map(arg.value(), newForOp.getRegionIterArgs()[arg.index()]);
    mapping.map(forOp.getInductionVar(), newForOp.getInductionVar());

    for (Operation &op : forOp.getBody()->getOperations()) {
      rewriter.clone(op, mapping);
    }
    rewriter.replaceOp(forOp, newForOp.getResults());
    return success();
  }
};

} // namespace

LogicalResult fixupLoops(ModuleOp mod) {
  auto *ctx = mod.getContext();
  mlir::RewritePatternSet patterns(ctx);
  patterns.add<FixupLoop>(ctx);
  if (applyPatternsAndFoldGreedily(mod, std::move(patterns)).failed())
    return failure();
  return success();
}

SmallVector<unsigned, 3> mmaVersionToInstrShape(int version,
                                                const ArrayRef<int64_t> &shape,
                                                RankedTensorType type) {
  if (version == 1)
    return {16, 16};
  else if (version == 2)
    return {16, 8};
  else if (version == 3) {
    unsigned k = 256 / type.getElementTypeBitWidth();
    if (shape[0] % 64 != 0 || shape[1] % 8 != 0) {
      assert(false && "type not supported");
      return {0, 0, 0};
    }
    auto eltType = type.getElementType();
    SmallVector<unsigned> validN;

    // MMAv3 with larger instruction shape is preferred.
    if (eltType.isFloat8E5M2() || eltType.isFloat8E4M3FN() || eltType.isF16() ||
        eltType.isBF16() || eltType.isF32()) {
      validN.assign({256, 248, 240, 232, 224, 216, 208, 200, 192, 184, 176,
                     168, 160, 152, 144, 136, 128, 120, 112, 104, 96,  88,
                     80,  72,  64,  56,  48,  40,  32,  24,  16,  8});
    }

    if (eltType.isInteger(8)) {
      validN.assign({224, 208, 192, 176, 160, 144, 128, 112, 96, 80, 64, 48, 32,
                     24, 16, 8});
    }

    for (auto n : validN) {
      if (shape[1] % n == 0) {
        return {16, n, k};
      }
    }

    assert(false && "type not supported");
    return {0, 0, 0};
  } else {
    assert(false && "version not supported");
    return {0, 0};
  }
}

bool isLoadFromTensorPtr(triton::LoadOp op) {
  return mlir::triton::isTensorPointerType(op.getPtr().getType());
}

bool isStoreToTensorPtr(triton::StoreOp op) {
  return mlir::triton::isTensorPointerType(op.getPtr().getType());
}

Operation *getFirstUser(Value v) {
  DenseMap<Operation *, size_t> operationId;
  v.getParentBlock()->walk<WalkOrder::PostOrder>(
      [&](Operation *op) { operationId[op] = operationId.size(); });
  size_t minId = std::numeric_limits<size_t>::max();
  Operation *firstUser = nullptr;
  for (Operation *user : v.getUsers()) {
    assert(operationId.find(user) != operationId.end());
    size_t userId = operationId[user];
    if (userId < minId) {
      minId = userId;
      firstUser = user;
    }
  }
  assert(firstUser);
  return firstUser;
}

triton::gpu::SharedEncodingAttr getSharedEncoding(RankedTensorType tensorTy) {
  auto blockedLayout =
      tensorTy.getEncoding().cast<triton::gpu::BlockedEncodingAttr>();
  return triton::gpu::SharedEncodingAttr::get(
      tensorTy.getContext(), tensorTy.getShape(), blockedLayout.getOrder(),
      blockedLayout.getCTALayout(), tensorTy.getElementType());
}

//===----------------------------------------------------------------------===//
// GraphDumper
//===----------------------------------------------------------------------===//

GraphDumper::NodeInfo GraphDumper::onValue(Value value) const {
  return {{"shape", "box"}, {"style", "filled"}, {"fillcolor", "white"}};
}

GraphDumper::NodeInfo GraphDumper::onOperation(Operation *op) const {
  return {{"shape", "ellipse"}, {"style", "filled"}, {"fillcolor", "white"}};
}

std::string GraphDumper::dump(triton::FuncOp func) const {
  llvm::SetVector<Value> values;
  llvm::SetVector<Operation *> operations;

  func.walk([&](Operation *op) {
    operations.insert(op);
    for (Value operand : op->getOperands())
      values.insert(operand);
    for (Value result : op->getResults())
      values.insert(result);
  });

  std::ostringstream oss;
  oss << "// Generated by Triton GraphDumper\n"
      << "\n"
      << "digraph {\n";

  oss << "    // Value Nodes\n";
  for (Value value : values)
    oss << "    " << emitValueNode(value) << "\n";
  oss << "\n";

  oss << "    // Operation Nodes\n";
  for (Operation *op : operations)
    oss << "    " << emitOperationNode(op) << "\n";
  oss << "\n";

  oss << "    // Edges\n";
  for (Operation *op : operations) {
    for (Value operand : op->getOperands())
      oss << "    " << emitEdge(getUniqueId(operand), getUniqueId(op)) << "\n";
    for (Value result : op->getResults())
      oss << "    " << emitEdge(getUniqueId(op), getUniqueId(result)) << "\n";
  }

  oss << "}\n";
  return oss.str();
}

void GraphDumper::dumpToFile(triton::FuncOp func,
                             const std::string &filename) const {
  std::ofstream ofs(filename);
  ofs << dump(func);
}

std::string GraphDumper::getShapeStr(const Type &type) const {
  std::ostringstream oss;
  oss << "[";
  if (auto tensorTy = type.dyn_cast<RankedTensorType>()) {
    auto shape = tensorTy.getShape();
    for (unsigned i = 0; i < shape.size(); ++i) {
      if (i > 0)
        oss << ", ";
      oss << shape[i];
    }
  }
  oss << "]";
  return oss.str();
}

std::string GraphDumper::getUniqueId(Value value) const {
  std::ostringstream oss;
  oss << value.getImpl();
  return oss.str();
}

std::string GraphDumper::getUniqueId(Operation *op) const {
  std::ostringstream oss;
  oss << op;
  return oss.str();
}

std::string GraphDumper::emitNode(const std::string &id,
                                  const GraphDumper::NodeInfo info) const {
  std::ostringstream oss;
  oss << "\"" << id << "\" [";
  for (auto it = info.begin(); it != info.end(); ++it) {
    if (it != info.begin())
      oss << ", ";
    oss << it->first << " = \"" << it->second << "\"";
  }
  oss << "];";
  return oss.str();
}

std::string GraphDumper::emitEdge(const std::string &srcId,
                                  const std::string &destId) const {
  std::ostringstream oss;
  oss << "\"" << srcId << "\" -> \"" << destId << "\";";
  return oss.str();
}

std::string GraphDumper::emitValueNode(Value value) const {
  NodeInfo info = onValue(value);
  if (info.find("label") == info.end()) {
    std::string shapeStr = getShapeStr(value.getType());
    if (auto arg = value.dyn_cast<BlockArgument>())
      info["label"] =
          "BlockArg" + std::to_string(arg.getArgNumber()) + " " + shapeStr;
    else
      info["label"] = shapeStr;
  }
  return emitNode(getUniqueId(value), info);
}

std::string GraphDumper::emitOperationNode(Operation *op) const {
  NodeInfo info = onOperation(op);
  if (info.find("label") == info.end())
    info["label"] = op->getName().getStringRef().str();
  return emitNode(getUniqueId(op), info);
}

//===----------------------------------------------------------------------===//
// GraphLayoutMarker
//===----------------------------------------------------------------------===//

GraphDumper::NodeInfo GraphLayoutMarker::onValue(Value value) const {
  std::string color = getColor(value.getType());
  return {{"shape", "box"}, {"style", "filled"}, {"fillcolor", color}};
}

std::string GraphLayoutMarker::getColor(const Type &type) const {
  if (auto tensorTy = type.dyn_cast<RankedTensorType>()) {
    auto layout = tensorTy.getEncoding();
    if (layout.isa<triton::gpu::BlockedEncodingAttr>())
      return "green";
    else if (layout.isa<triton::gpu::SliceEncodingAttr>())
      return "yellow";
    else if (layout.isa<triton::gpu::MmaEncodingAttr>())
      return "lightslateblue";
    else if (layout.isa<triton::gpu::DotOperandEncodingAttr>())
      return "orange";
    else if (layout.isa<triton::gpu::SharedEncodingAttr>())
      return "orangered";
    else
      assert(0 && "Unrecognized layout");
  } else {
    return "white";
  }
}
// -------------------------------------------------------------------------- //

// TODO: Interface
LogicalResult invertEncoding(Attribute targetEncoding, Operation *op,
                             Attribute &ret) {
  ret = targetEncoding;
  if (auto expand_dims = dyn_cast<triton::ExpandDimsOp>(op)) {
    ret = triton::gpu::SliceEncodingAttr::get(
        op->getContext(), expand_dims.getAxis(), targetEncoding);
  }
  if (auto reduce = dyn_cast<triton::ReduceOp>(op)) {
    auto sliceEncoding =
        targetEncoding.dyn_cast<triton::gpu::SliceEncodingAttr>();
    if (!sliceEncoding)
      return failure();
    if (sliceEncoding.getDim() != reduce.getAxis())
      return failure();
    ret = sliceEncoding.getParent();
  }
  if (isa<triton::ViewOp, triton::CatOp>(op)) {
    return failure();
  }
  return success();
}

bool isExpensiveLoadOrStore(Operation *op, Attribute &targetEncoding) {
  // Case 1: A size 1 tensor is not expensive since all threads will load the
  // same
  if (isSingleValue(op->getOperand(0)))
    return false;
  // Case 2: Tensor of pointers has more threads than elements
  // we can presume a high hit-rate that makes it cheap to load
  auto ptrType = op->getOperand(0).getType().cast<RankedTensorType>();
  auto mod = op->getParentOfType<ModuleOp>();
  int numWarps = triton::gpu::TritonGPUDialect::getNumWarps(mod);
  int threadsPerWarp = triton::gpu::TritonGPUDialect::getThreadsPerWarp(mod);
  if (ptrType.getNumElements() < numWarps * threadsPerWarp)
    return false;
  return true;
}

bool isExpensiveToRemat(Operation *op, Attribute &targetEncoding) {
  if (!op)
    return true;
  if (isa<triton::LoadOp, triton::StoreOp>(op))
    return isExpensiveLoadOrStore(op, targetEncoding);
  if (isa<triton::CatOp>(op))
    return triton::gpu::isExpensiveCat(cast<triton::CatOp>(op), targetEncoding);
  if (isa<tensor::ExtractSliceOp, triton::gpu::AllocTensorOp,
          triton::gpu::InsertSliceAsyncOp, triton::AtomicRMWOp,
          triton::AtomicCASOp, triton::DotOp>(op))
    return true;
  if (isa<scf::YieldOp, scf::ForOp, scf::IfOp, scf::WhileOp, scf::ConditionOp>(
          op))
    return true;
  return false;
}

bool canFoldConversion(Operation *op, Attribute targetEncoding) {
  if (isa<triton::CatOp>(op))
    return !triton::gpu::isExpensiveCat(cast<triton::CatOp>(op),
                                        targetEncoding);
  return isa<triton::gpu::ConvertLayoutOp, arith::ConstantOp,
             triton::MakeRangeOp, triton::SplatOp, triton::ViewOp>(op);
}

int simulateBackwardRematerialization(
    Operation *initOp, SetVector<Operation *> &processed,
    SetVector<Attribute> &layout, llvm::MapVector<Value, Attribute> &toConvert,
    Attribute targetEncoding) {
  // DFS
  std::vector<std::pair<Operation *, Attribute>> queue;
  queue.emplace_back(initOp, targetEncoding);
  // We want to see the effect of converting `initOp` to a new layout
  // so we initialize `numCvts = 1`.
  int numCvts = 1;
  while (!queue.empty()) {
    Operation *currOp;
    Attribute currLayout;
    std::tie(currOp, currLayout) = queue.back();
    queue.pop_back();
    // If the current operation is expensive to rematerialize,
    // we stop everything
    if (isExpensiveToRemat(currOp, currLayout))
      break;
    // A conversion will be removed here (i.e. transferred to operands)
    numCvts -= 1;
    // Done processing
    processed.insert(currOp);
    layout.insert(currLayout);
    // Add all operands to the queue
    for (Value argI : currOp->getOperands()) {
      Attribute newEncoding;
      // Cannot invert the current encoding for this operand
      // we stop everything
      if (failed(invertEncoding(currLayout, currOp, newEncoding)))
        return INT_MAX;
      if (toConvert.count(argI) && toConvert[argI] != newEncoding)
        return INT_MAX;
      if (auto ptrTy = argI.getType().dyn_cast<triton::PointerType>()) {
        if (ptrTy.getPointeeType().isa<RankedTensorType>()) {
          return INT_MAX;
        }
      }

      Operation *opArgI = argI.getDefiningOp();
      toConvert.insert({argI, newEncoding});
      // 1. Only convert RankedTensorType
      // 2. Skip if there's no defining op
      // 3. Skip if the defining op has already been processed
      // 4. Skip or the defining op is in a different block
      if (!argI.getType().isa<RankedTensorType>() || !opArgI ||
          processed.contains(opArgI) ||
          opArgI->getBlock() != currOp->getBlock())
        continue;
      // If the conversion can be folded into opArgI then
      // we don't count this conversion as expensive
      if (canFoldConversion(opArgI, newEncoding))
        continue;

      // We add one expensive conversion for the current operand
      numCvts += 1;
      queue.emplace_back(opArgI, newEncoding);
    }
  }
  // return net number of conversions
  return numCvts;
}

//

Operation *cloneWithInferType(mlir::OpBuilder &rewriter, Operation *op,
                              IRMapping &mapping) {
  Operation *newOp = rewriter.clone(*op, mapping);
  // if input types haven't changed, we're done
  bool preserveTypes =
      std::all_of(op->operand_begin(), op->operand_end(), [&](Value v) {
        return !mapping.contains(v) ||
               v.getType() == mapping.lookup(v).getType();
      });
  if (preserveTypes)
    return newOp;

  if (newOp->getNumResults() == 0)
    return newOp;
  auto origType = op->getResult(0).getType().dyn_cast<RankedTensorType>();
  auto argType = newOp->getOperand(0).getType().dyn_cast<RankedTensorType>();
  if (!origType || !argType)
    return newOp;
  auto newType = RankedTensorType::get(
      origType.getShape(), origType.getElementType(), argType.getEncoding());
  newOp->getResult(0).setType(newType);
  auto typeInfer = dyn_cast<InferTypeOpInterface>(newOp);
  if (typeInfer) {
    SmallVector<Type, 1> newTypes;
    auto success = typeInfer.inferReturnTypes(
        newOp->getContext(), newOp->getLoc(), newOp->getOperands(),
        newOp->getAttrDictionary(), newOp->getPropertiesStorage(),
        newOp->getRegions(), newTypes);
    if (succeeded(success)) {
      for (size_t i = 0; i < newTypes.size(); i++)
        newOp->getResult(i).setType(newTypes[i]);
    }
  }
  return newOp;
}

namespace {

struct OpUseInfo {
  Value value;
  Operation *op;
  unsigned index;
};

void getForwardSliceOpUseInfo(Operation *op,
                              SetVector<Operation *> *forwardSliceOps,
                              SmallVector<OpUseInfo> *forwardOpUseInfo) {
  if (!op)
    return;

  for (Region &region : op->getRegions())
    for (Block &block : region)
      for (Operation &blockOp : block)
        if (forwardSliceOps->count(&blockOp) == 0)
          getForwardSliceOpUseInfo(&blockOp, forwardSliceOps, forwardOpUseInfo);
  for (Value result : op->getResults()) {
    for (OpOperand &operand : result.getUses()) {
      auto *blockOp = operand.getOwner();
      forwardOpUseInfo->push_back(
          {operand.get(), blockOp, operand.getOperandNumber()});
      if (forwardSliceOps->count(blockOp) == 0)
        getForwardSliceOpUseInfo(blockOp, forwardSliceOps, forwardOpUseInfo);
    }
  }

  forwardSliceOps->insert(op);
}
} // namespace

LogicalResult simulateForwardRematerializationInLoop(Operation *startOp,
                                                     BlockArgument arg,
                                                     Attribute targetEncoding) {
  // heuristics for flash attention
  if (targetEncoding.isa<triton::gpu::SharedEncodingAttr,
                         triton::gpu::SliceEncodingAttr>())
    return failure();
  SetVector<Operation *> cvtSliceOps;
  SmallVector<OpUseInfo> cvtSliceOpUseInfo;
  getForwardSliceOpUseInfo(startOp, &cvtSliceOps, &cvtSliceOpUseInfo);

  // Check if any additional conversion is needed along the way
  for (Operation *op : cvtSliceOps) {
    if (isa<scf::YieldOp>(op))
      continue;
    // The first op doesn't push forward any conversion
    if (op != startOp) {
      // don't rematerialize anything expensive
      if (isExpensiveToRemat(op, targetEncoding))
        return failure();
      // don't rematerialize non-element-wise
      if (!op->hasTrait<mlir::OpTrait::SameOperandsAndResultEncoding>() &&
          !op->hasTrait<mlir::OpTrait::Elementwise>() &&
          !isa<triton::StoreOp, triton::AssertOp, triton::PrintOp,
               triton::ReduceOp>(op))
        return failure();
    }
    // don't rematerialize if it adds an extra conversion that can't
    // be removed
    for (Value value : op->getOperands()) {
      Operation *argOp = arg.getDefiningOp();
      SetVector<Operation *> processed;
      SetVector<Attribute> layout;
      llvm::MapVector<Value, Attribute> toConvert;
      int numAddedConvs = simulateBackwardRematerialization(
          argOp, processed, layout, toConvert, targetEncoding);
      if (argOp && !isa<triton::gpu::ConvertLayoutOp>(argOp) &&
          cvtSliceOps.count(argOp) == 0 && numAddedConvs > 0)
        return failure();
    }
  }

  // We apply conservative analysis. Only when the final operand's index
  // matches the argument's index or their encoding match, we can rematerialize.
  for (auto &opUseInfo : cvtSliceOpUseInfo) {
    Operation *op = opUseInfo.op;
    if (isa<scf::YieldOp>(op)) {
      auto yieldIdx = opUseInfo.index;
      // 0 is the induction variable
      auto argIdx = arg.getArgNumber() - 1;
      if (yieldIdx != argIdx) {
        auto argType = arg.getType().cast<RankedTensorType>();
        auto yieldType =
            op->getOperand(yieldIdx).getType().dyn_cast<RankedTensorType>();
        if (!yieldType || argType.getEncoding() != yieldType.getEncoding())
          return failure();
      }
    }
  }
  return success();
}

void rematerializeConversionChain(
    const llvm::MapVector<Value, Attribute> &toConvert,
    mlir::PatternRewriter &rewriter, SetVector<Operation *> &processed,
    IRMapping &mapping) {
  SmallVector<Value, 4> sortedValues;
  SetVector<Operation *> tmp;
  for (auto &item : toConvert) {
    Value v = item.first;
    if (v.getDefiningOp())
      tmp.insert(v.getDefiningOp());
    else
      sortedValues.push_back(v);
  }
  tmp = mlir::multiRootTopologicalSort(tmp);
  for (Operation *op : tmp)
    sortedValues.push_back(op->getResult(0));

  for (Value currOperand : sortedValues) {
    Value origOperand = currOperand;
    // unpack information
    Attribute targetLayout = toConvert.lookup(currOperand);
    // rematerialize the operand if necessary
    Operation *currOperation = currOperand.getDefiningOp();
    if (processed.contains(currOperation)) {
      Operation *newOperation =
          cloneWithInferType(rewriter, currOperation, mapping);
      newOperation->moveAfter(currOperation);
      currOperation = newOperation;
      currOperand = currOperation->getResult(0);
    }
    // compute target type for the layout cast
    auto currType = currOperand.getType().cast<RankedTensorType>();
    auto newType = RankedTensorType::get(
        currType.getShape(), currType.getElementType(), targetLayout);
    auto newOperand = rewriter.create<triton::gpu::ConvertLayoutOp>(
        currOperand.getLoc(), newType, currOperand);
    if (currOperation)
      newOperand->moveAfter(currOperation);
    else {
      Block *block = currOperand.cast<BlockArgument>().getOwner();
      newOperand->moveBefore(block, block->begin());
    }
    mapping.map(origOperand, newOperand);
  }
}

LogicalResult canMoveOutOfLoop(BlockArgument arg,
                               SmallVector<Operation *> &cvts) {
  auto parentOp = arg.getOwner()->getParentOp();
  // Don't move if arg is defined in a while loop
  if (isa<scf::WhileOp>(parentOp))
    return failure();
  // Skip if arg is not defined in scf.for
  if (!isa<scf::ForOp>(parentOp))
    return success();
  auto forOp = cast<scf::ForOp>(parentOp);
  // We only move `iterArg` out of the loop if
  // 1. There is no conversion
  // 2. There is only a single conversion
  // 3. Moving this conversion out of the loop will not generate any extra
  // non-removable conversion
  SetVector<RankedTensorType> cvtTypes;
  SetVector<Operation *> others;
  auto oldType = arg.getType().cast<RankedTensorType>();
  for (auto user : arg.getUsers()) {
    if (isa<triton::gpu::ConvertLayoutOp>(user)) {
      // Don't move if the conversion target is a dot operand or shared memory
      auto newType = user->getResults()[0].getType().cast<RankedTensorType>();
      if (oldType.getEncoding().isa<triton::gpu::SharedEncodingAttr>() &&
          newType.getEncoding().isa<triton::gpu::DotOperandEncodingAttr>()) {
        continue;
      }
      if (newType.getEncoding().isa<triton::gpu::SharedEncodingAttr>()) {
        if (newType.getEncoding()
                .cast<triton::gpu::SharedEncodingAttr>()
                .getVec() == 1)
          continue;
      }
      cvts.emplace_back(user);
      cvtTypes.insert(newType);
    } else
      others.insert(user);
  }
  // First condition
  if (cvts.empty())
    return success();
  if (cvtTypes.size() == 1) {
    // Third condition - part 1:
    // If the other or the cvt is in the different block, we cannot push the
    // conversion forward or backward
    for (auto *cvt : cvts) {
      if (cvt->getBlock() != forOp.getBody())
        return failure();
    }
    auto targetEncoding = cvtTypes.front().getEncoding();
    for (auto *other : others) {
      // Third condition - part 2:
      // If the other non-cvt op is in the different block, we cannot push the
      // conversion forward or backward
      if (other->getBlock() != forOp.getBody())
        return failure();
      // Third condition - part 3:
      // Check if we can directly use arg without conversion
      if (simulateForwardRematerializationInLoop(other, arg, targetEncoding)
              .failed())
        return failure();
    }
    return success();
  }
  return failure();
}

// TODO(thomas): this is duplicated with what is in GPUToLLVM
//  Convert an \param index to a multi-dim coordinate given \param shape and
//  \param order.
SmallVector<Value> delinearize(OpBuilder &b, Location loc, Value linear,
                               ArrayRef<unsigned> shape,
                               ArrayRef<unsigned> order) {
  unsigned rank = shape.size();
  assert(rank == order.size());
  auto reordered = reorder(shape, order);
  auto reorderedMultiDim = delinearize(b, loc, linear, reordered);
  SmallVector<Value> multiDim(rank);
  for (unsigned i = 0; i < rank; ++i) {
    multiDim[order[i]] = reorderedMultiDim[i];
  }
  return multiDim;
}

SmallVector<Value> delinearize(OpBuilder &b, Location loc, Value linear,
                               ArrayRef<unsigned> shape) {
  unsigned rank = shape.size();
  assert(rank > 0);
  SmallVector<Value> multiDim(rank);
  if (rank == 1) {
    multiDim[0] = linear;
  } else {
    Value remained = linear;
    for (auto &&en : llvm::enumerate(shape.drop_back())) {
      auto dimSize = b.create<arith::ConstantIntOp>(loc, en.value(), 32);
      multiDim[en.index()] = b.create<arith::RemSIOp>(loc, remained, dimSize);
      remained = b.create<arith::DivSIOp>(loc, remained, dimSize);
    }
    multiDim[rank - 1] = remained;
  }
  return multiDim;
}

Value linearize(OpBuilder &b, Location loc, ArrayRef<Value> multiDim,
                ArrayRef<unsigned> shape, ArrayRef<unsigned> order) {
  return linearize(b, loc, reorder<Value>(multiDim, order),
                   reorder<unsigned>(shape, order));
}

Value linearize(OpBuilder &b, Location loc, ArrayRef<Value> multiDim,
                ArrayRef<unsigned> shape) {
  auto rank = multiDim.size();
  Value linear = b.create<arith::ConstantIntOp>(loc, 0, 32);
  if (rank > 0) {
    linear = multiDim.back();
    for (auto [dim, dimShape] :
         llvm::reverse(llvm::zip(multiDim.drop_back(), shape.drop_back()))) {
      Value dimSize = b.create<arith::ConstantIntOp>(loc, dimShape, 32);
      linear = b.create<arith::AddIOp>(
          loc, b.create<arith::MulIOp>(loc, linear, dimSize), dim);
    }
  }
  return linear;
}

} // namespace mlir
