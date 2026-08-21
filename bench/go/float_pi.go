package main

import "fmt"

func main() {
	n := 100000000
	sum := 0.0
	for i := 0; i < n; i++ {
		d := float64(i*2 + 1)
		if i%2 == 0 {
			sum += 1.0 / d
		} else {
			sum -= 1.0 / d
		}
	}
	pi := sum * 4.0
	fmt.Println(pi)
}
