package main

import "fmt"

func main() {
	s := "the quick brown fox jumps over the lazy dog"
	total := 0
	for iter := 0; iter < 200000; iter++ {
		out := ""
		for i := len(s) - 1; i >= 0; i-- {
			out += string(s[i])
		}
		total += len(out)
	}
	fmt.Println(total)
}
