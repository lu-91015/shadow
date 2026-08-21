package main

import "fmt"

func partition(a []int, lo, hi int) int {
	pivot := a[hi]
	i := lo - 1
	for j := lo; j < hi; j++ {
		if a[j] < pivot {
			i++
			a[i], a[j] = a[j], a[i]
		}
	}
	a[i+1], a[hi] = a[hi], a[i+1]
	return i + 1
}

func qs(a []int, lo, hi int) {
	if lo < hi {
		p := partition(a, lo, hi)
		qs(a, lo, p-1)
		qs(a, p+1, hi)
	}
}

func main() {
	n := 20000
	a := make([]int, n)
	for i := 0; i < n; i++ {
		a[i] = (n - i) % 7919
	}
	qs(a, 0, n-1)
	fmt.Println(a[0])
	fmt.Println(a[n-1])
}
