//===- GenericDomTreeConstruction.h - Dominator Calculation ------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// Generic dominator tree construction - This file provides routines to
/// construct immediate dominator information for a flow-graph based on the
/// Semi-NCA algorithm described in this dissertation:
///
///   Linear-Time Algorithms for Dominators and Related Problems
///   Loukas Georgiadis, Princeton University, November 2005, pp. 21-23:
///   ftp://ftp.cs.princeton.edu/reports/2005/737.pdf
///
/// This implements the O(n*log(n)) versions of EVAL and LINK, because it turns
/// out that the theoretically slower O(n*log(n)) implementation is actually
/// faster than the almost-linear O(n*alpha(n)) version, even for large CFGs.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_GENERICDOMTREECONSTRUCTION_H
#define LLVM_SUPPORT_GENERICDOMTREECONSTRUCTION_H

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/GenericDomTree.h"

namespace llvm {

// External storage for depth first iterator that reuses the info lookup map
// domtree already has.  We don't have a set, but a map instead, so we are
// converting the one argument insert calls.
template <class NodeRef, class InfoType> struct df_iterator_dom_storage {
public:
  using BaseSet = DenseMap<NodeRef, InfoType>;
  df_iterator_dom_storage(BaseSet &Storage) : Storage(Storage) {}

  using iterator = typename BaseSet::iterator;
  std::pair<iterator, bool> insert(NodeRef N) {
    return Storage.insert({N, InfoType()});
  }
  void completed(NodeRef) {}

private:
  BaseSet &Storage;
};

template <class GraphT>
unsigned ReverseDFSPass(
    DominatorTreeBaseByGraphTraits<GraphT> &DT, typename GraphT::NodeRef V,
    typename DominatorTreeBaseByGraphTraits<GraphT>::SemiNCAInfo &SNCA,
    unsigned N) {
  using SNCAInfoTy = typename std::remove_reference<decltype(SNCA)>::type;
  df_iterator_dom_storage<typename SNCAInfoTy::NodePtr,
      typename SNCAInfoTy::InfoRec>
      DFStorage(SNCA.NodeToInfo);

  bool IsChildOfArtificialExit = (N != 0);
  for (auto I = idf_ext_begin(V, DFStorage), E = idf_ext_end(V, DFStorage);
       I != E; ++I) {
    typename GraphT::NodeRef BB = *I;
    auto &BBInfo = SNCA.NodeToInfo[BB];
    BBInfo.DFSNum = BBInfo.Semi = ++N;
    BBInfo.Label = BB;
    // Set the parent to the top of the visited stack.  The stack includes us,
    // and is 1 based, so we subtract to account for both of these.
    if (I.getPathLength() > 1)
      BBInfo.Parent = SNCA.NodeToInfo[I.getPath(I.getPathLength() - 2)].DFSNum;
    SNCA.NumToNode.push_back(BB);  // NumToNode[n] = V;

    if (IsChildOfArtificialExit)
      BBInfo.Parent = 1;

    IsChildOfArtificialExit = false;
  }
  return N;
}
template <class GraphT>
unsigned DFSPass(
    DominatorTreeBaseByGraphTraits<GraphT> &DT, typename GraphT::NodeRef V,
    typename DominatorTreeBaseByGraphTraits<GraphT>::SemiNCAInfo &SNCA,
    unsigned N) {
  using SNCAInfoTy = typename std::remove_reference<decltype(SNCA)>::type;
  df_iterator_dom_storage<typename SNCAInfoTy::NodePtr,
      typename SNCAInfoTy::InfoRec>
      DFStorage(SNCA.NodeToInfo);
  for (auto I = df_ext_begin(V, DFStorage), E = df_ext_end(V, DFStorage);
       I != E; ++I) {
    typename GraphT::NodeRef BB = *I;
    auto &BBInfo = SNCA.NodeToInfo[BB];
    BBInfo.DFSNum = BBInfo.Semi = ++N;
    BBInfo.Label = BB;
    // Set the parent to the top of the visited stack.  The stack includes us,
    // and is 1 based, so we subtract to account for both of these.
    if (I.getPathLength() > 1)
      BBInfo.Parent = SNCA.NodeToInfo[I.getPath(I.getPathLength() - 2)].DFSNum;
    SNCA.NumToNode.push_back(BB);  // NumToNode[n] = V;
  }
  return N;
}

template <class GraphT>
typename GraphT::NodeRef Eval(
    DominatorTreeBaseByGraphTraits<GraphT> &DT, typename GraphT::NodeRef VIn,
    typename DominatorTreeBaseByGraphTraits<GraphT>::SemiNCAInfo &SNCA,
    unsigned LastLinked) {
  using NodePtr = typename GraphT::NodeRef;

  auto &VInInfo = SNCA.NodeToInfo[VIn];
  if (VInInfo.DFSNum < LastLinked)
    return VIn;

  SmallVector<NodePtr, 32> Work;
  SmallPtrSet<NodePtr, 32> Visited;

  if (VInInfo.Parent >= LastLinked)
    Work.push_back(VIn);

  while (!Work.empty()) {
    NodePtr V = Work.back();
    auto &VInfo = SNCA.NodeToInfo[V];
    NodePtr VAncestor = SNCA.NumToNode[VInfo.Parent];

    // Process Ancestor first
    if (Visited.insert(VAncestor).second && VInfo.Parent >= LastLinked) {
      Work.push_back(VAncestor);
      continue;
    }
    Work.pop_back();

    // Update VInfo based on Ancestor info
    if (VInfo.Parent < LastLinked)
      continue;

    auto &VAInfo = SNCA.NodeToInfo[VAncestor];
    NodePtr VAncestorLabel = VAInfo.Label;
    NodePtr VLabel = VInfo.Label;
    if (SNCA.NodeToInfo[VAncestorLabel].Semi < SNCA.NodeToInfo[VLabel].Semi)
      VInfo.Label = VAncestorLabel;
    VInfo.Parent = VAInfo.Parent;
  }

  return VInInfo.Label;
}

template <class FuncT, class NodeT>
void Calculate(DominatorTreeBaseByGraphTraits<GraphTraits<NodeT>> &DT,
               FuncT &F) {
  using GraphT = GraphTraits<NodeT>;
  using NodePtr = typename GraphT::NodeRef;
  static_assert(std::is_pointer<NodePtr>::value,
                "NodeRef should be pointer type");
  using NodeType = typename std::remove_pointer<NodePtr>::type;

  unsigned N = 0;
  typename DominatorTreeBaseByGraphTraits<GraphT>::SemiNCAInfo SNCA;
  SNCA.NumToNode.push_back(nullptr);

  bool MultipleRoots = (DT.Roots.size() > 1);
  if (MultipleRoots) {
    auto &BBInfo = SNCA.NodeToInfo[nullptr];
    BBInfo.DFSNum = BBInfo.Semi = ++N;
    BBInfo.Label = nullptr;

    SNCA.NumToNode.push_back(nullptr); // NumToNode[n] = V;
  }

  // Step #1: Number blocks in depth-first order and initialize variables used
  // in later stages of the algorithm.
  if (DT.isPostDominator()){
    for (unsigned i = 0, e = static_cast<unsigned>(DT.Roots.size());
         i != e; ++i)
      N = ReverseDFSPass<GraphT>(DT, DT.Roots[i], SNCA, N);
  } else {
    N = DFSPass<GraphT>(DT, DT.Roots[0], SNCA, N);
  }

  // It might be that some blocks did not get a DFS number (e.g., blocks of
  // infinite loops). In these cases an artificial exit node is required.
  MultipleRoots |= (DT.isPostDominator() && N != GraphTraits<FuncT*>::size(&F));

  // Initialize IDoms to spanning tree parents.
  for (unsigned i = 1; i <= N; ++i) {
    const NodePtr V = SNCA.NumToNode[i];
    DT.IDoms[V] = SNCA.NumToNode[SNCA.NodeToInfo[V].Parent];
  }

  // Step #2: Calculate the semidominators of all vertices.
  for (unsigned i = N; i >= 2; --i) {
    NodePtr W = SNCA.NumToNode[i];
    auto &WInfo = SNCA.NodeToInfo[W];

    // Initialize the semi dominator to point to the parent node.
    WInfo.Semi = WInfo.Parent;
    for (const auto &N : inverse_children<NodeT>(W))
      if (SNCA.NodeToInfo.count(N)) {  // Only if this predecessor is reachable!
        unsigned SemiU = SNCA.NodeToInfo[Eval<GraphT>(DT, N, SNCA, i + 1)].Semi;
        if (SemiU < WInfo.Semi)
          WInfo.Semi = SemiU;
      }
  }


  // Step #3: Explicitly define the immediate dominator of each vertex.
  //          IDom[i] = NCA(SDom[i], SpanningTreeParent(i)).
  // Note that the parents were stored in IDoms and later got invalidated during
  // path compression in Eval.
  for (unsigned i = 2; i <= N; ++i) {
    const NodePtr W = SNCA.NumToNode[i];
    const auto &WInfo = SNCA.NodeToInfo[W];
    const unsigned SDomNum = SNCA.NodeToInfo[SNCA.NumToNode[WInfo.Semi]].DFSNum;
    NodePtr WIDomCandidate = DT.IDoms[W];
    while (SNCA.NodeToInfo[WIDomCandidate].DFSNum > SDomNum)
      WIDomCandidate = DT.IDoms[WIDomCandidate];

    DT.IDoms[W] = WIDomCandidate;
  }

  if (DT.Roots.empty()) return;

  // Add a node for the root.  This node might be the actual root, if there is
  // one exit block, or it may be the virtual exit (denoted by (BasicBlock *)0)
  // which postdominates all real exits if there are multiple exit blocks, or
  // an infinite loop.
  NodePtr Root = !MultipleRoots ? DT.Roots[0] : nullptr;

  DT.RootNode =
      (DT.DomTreeNodes[Root] =
           llvm::make_unique<DomTreeNodeBase<NodeType>>(Root, nullptr))
          .get();

  // Loop over all of the reachable blocks in the function...
  for (unsigned i = 2; i <= N; ++i) {
    NodePtr W = SNCA.NumToNode[i];

    // Don't replace this with 'count', the insertion side effect is important
    if (DT.DomTreeNodes[W])
      continue; // Haven't calculated this node yet?

    NodePtr ImmDom = DT.getIDom(W);

    assert(ImmDom || DT.DomTreeNodes[nullptr]);

    // Get or calculate the node for the immediate dominator
    DomTreeNodeBase<NodeType> *IDomNode = DT.getNodeForBlock(ImmDom);

    // Add a new tree node for this BasicBlock, and link it as a child of
    // IDomNode
    DT.DomTreeNodes[W] = IDomNode->addChild(
        llvm::make_unique<DomTreeNodeBase<NodeType>>(W, IDomNode));
  }

  // Free temporary memory used to construct idom's
  DT.IDoms.clear();

  DT.updateDFSNumbers();
}
}

#endif
