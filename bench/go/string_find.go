package main

import (
	"fmt"
	"strings"
)

func main() {
	s := "the quick brown fox jumps over the lazy dog"
	needle := "lazy"
	total := 0
	for i := 0; i < 300000; i++ {
		total += strings.Index(s, needle)
	}
	fmt.Println(total)
}
