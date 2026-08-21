package main

import "fmt"

func main() {
	a := make([]int, 0, 8)
	for i := 0; i < 1000000; i++ {
		a = append(a, i)
	}
	fmt.Println(len(a))
	fmt.Println(a[999999])
}
