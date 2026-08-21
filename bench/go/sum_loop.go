package main

import "fmt"

func main() {
	n := 100000000
	s := 0
	for i := 0; i < n; i++ {
		s += i
	}
	fmt.Println(s)
}
