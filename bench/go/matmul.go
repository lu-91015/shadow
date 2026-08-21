package main

import "fmt"

func main() {
	n := 150
	a := make([][]int, n)
	b := make([][]int, n)
	c := make([][]int, n)
	for i := 0; i < n; i++ {
		a[i] = make([]int, n)
		b[i] = make([]int, n)
		c[i] = make([]int, n)
		for j := 0; j < n; j++ {
			a[i][j] = i*n + j
			b[i][j] = j*n + i
		}
	}
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			s := 0
			for k := 0; k < n; k++ {
				s += a[i][k] * b[k][j]
			}
			c[i][j] = s
		}
	}
	fmt.Println(c[0][0])
	fmt.Println(c[n-1][n-1])
}
