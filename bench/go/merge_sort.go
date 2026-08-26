package main

import "fmt"

func merge(a, tmp []int, lo, mid, hi int) {
	i, j, k := lo, mid, lo
	for i < mid && j <= hi {
		if a[i] <= a[j] {
			tmp[k] = a[i]
			i++
		} else {
			tmp[k] = a[j]
			j++
		}
		k++
	}
	for i < mid {
		tmp[k] = a[i]
		i++
		k++
	}
	for j <= hi {
		tmp[k] = a[j]
		j++
		k++
	}
	for c := lo; c <= hi; c++ {
		a[c] = tmp[c]
	}
}

func msort(a, tmp []int, lo, hi int) {
	if lo < hi {
		mid := (lo + hi) / 2
		msort(a, tmp, lo, mid)
		msort(a, tmp, mid+1, hi)
		merge(a, tmp, lo, mid+1, hi)
	}
}

func main() {
	n := 100000
	a := make([]int, n)
	for i := 0; i < n; i++ {
		a[i] = (n - i) % 7919
	}
	tmp := make([]int, n)
	msort(a, tmp, 0, n-1)
	fmt.Printf("%d\n%d\n", a[0], a[n-1])
}
