package main

import "fmt"

type TreeNode struct{ left, right *TreeNode }

func bottomUpTree(depth int32) *TreeNode {
	if depth == 0 {
		return &TreeNode{}
	}
	return &TreeNode{bottomUpTree(depth - 1), bottomUpTree(depth - 1)}
}

func itemCheck(node *TreeNode) int32 {
	if node.left == nil || node.right == nil {
		return 1
	}
	return 1 + itemCheck(node.left) + itemCheck(node.right)
}

func main() {
	const maxDepth = 18
	const stretchDepth = maxDepth + 1
	fmt.Printf("stretch tree of depth %d\t check: %d\n", stretchDepth, itemCheck(bottomUpTree(stretchDepth)))
	longLivedTree := bottomUpTree(maxDepth)
	for depth := int32(4); depth <= maxDepth; depth += 2 {
		iterations := int32(1) << (maxDepth - depth + 4)
		check := int32(0)
		for i := int32(0); i < iterations; i++ {
			check += itemCheck(bottomUpTree(depth))
		}
		fmt.Printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check)
	}
	fmt.Printf("long lived tree of depth %d\t check: %d\n", maxDepth, itemCheck(longLivedTree))
}
