package main

import "fmt"

func main() {
	s := ""
	for i := 0; i < 20000; i++ {
		s += "abc"
	}
	fmt.Println(len(s))
}
