package main

import "fmt"

type node struct {
	left, right *node
	item        int
}

func (n *node) check() int {
	if n.left == nil {
		return n.item
	}
	return n.item + n.left.check() + n.right.check()
}

func buildTree(depth int) *node {
	nn := &node{item: 1}
	if depth > 0 {
		nn.left = buildTree(depth - 1)
		nn.right = buildTree(depth - 1)
	}
	return nn
}

func main() {
	const minDepth = 4
	maxDepth := 10
	longLivedTree := buildTree(maxDepth)
	total := 0
	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := 1 << uint(maxDepth-depth)
		sum := 0
		for i := 1; i <= iterations; i++ {
			a := buildTree(depth)
			sum += a.check()
		}
		total += sum
	}
	fmt.Printf("%d\n%d\n", longLivedTree.check(), total)
}
