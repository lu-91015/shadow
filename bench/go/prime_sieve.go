package main

import "fmt"

func main() {
	n := 2000000
	a := make([]int, n)
	for i := 0; i < n; i++ {
		a[i] = 1
	}
	for p := 2; p*p < n; p++ {
		if a[p] == 1 {
			for j := p * p; j < n; j += p {
				a[j] = 0
			}
		}
	}
	count := 0
	for i := 2; i < n; i++ {
		if a[i] == 1 {
			count++
		}
	}
	fmt.Println(count)
}
