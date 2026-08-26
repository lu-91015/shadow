package main

import (
	"fmt"
	"math"
)

func evalA(i, j int) float64 {
	if (i+j)%2 == 0 {
		return 1.0 / float64(i+j+1)
	}
	return -1.0 / float64(i+j+1)
}

func multiplyAv(v, tmp []float64, n int) {
	for i := 0; i < n; i++ {
		var s float64
		for j := 0; j < n; j++ {
			s += evalA(i, j) * v[j]
		}
		tmp[i] = s
	}
	for i := 0; i < n; i++ {
		v[i] = tmp[i]
	}
}

func multiplyAtv(v, tmp []float64, n int) {
	for i := 0; i < n; i++ {
		var s float64
		for j := 0; j < n; j++ {
			s += evalA(j, i) * v[j]
		}
		tmp[i] = s
	}
	for i := 0; i < n; i++ {
		v[i] = tmp[i]
	}
}

func multiplyAtAv(v, out, tmp []float64, n int) {
	multiplyAv(v, tmp, n)
	multiplyAtv(tmp, out, n)
}

func main() {
	n := 100
	u := make([]float64, n)
	for i := 0; i < n; i++ {
		u[i] = 1.0
	}
	v := make([]float64, n)
	tmp := make([]float64, n)
	for i := 0; i < 10; i++ {
		multiplyAtAv(u, v, tmp, n)
		multiplyAtAv(v, u, tmp, n)
	}
	var vv, vBv float64
	for i := 0; i < n; i++ {
		vv += u[i] * u[i]
		vBv += u[i] * v[i]
	}
	fmt.Printf("%.9f\n", math.Sqrt(vBv/vv))
}
