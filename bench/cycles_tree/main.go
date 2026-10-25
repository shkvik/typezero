package main

import "fmt"

type TreeNode struct {
	children []*TreeNode
	parent   *TreeNode
	id       int32
}

var nextId int32

func build(depth int32, parent *TreeNode) *TreeNode {
	node := &TreeNode{id: nextId}
	nextId++
	if parent != nil {
		node.parent = parent
		parent.children = append(parent.children, node)
	}
	if depth > 0 {
		for i := 0; i < 4; i++ {
			build(depth-1, node)
		}
	}
	return node
}

func walk(node *TreeNode) int32 {
	if len(node.children) == 0 {
		depth := int32(0)
		for up := node.parent; up != nil; up = up.parent {
			depth++
		}
		return depth
	}
	sum := int32(0)
	for _, c := range node.children {
		sum += walk(c)
	}
	return sum
}

func main() {
	check := int32(0)
	for round := 0; round < 10; round++ {
		nextId = 0
		root := build(9, nil)
		check += walk(root) + nextId
	}
	fmt.Println(check)
}
