package main

import "fmt"

func main() {
	n := 10
	perm1 := make([]int, n)
	count := make([]int, n)
	for i := 0; i < n; i++ {
		perm1[i] = i + 1
	}
	var maxFlips, checksum int
	r := n
	for {
		if perm1[0] != 1 {
			k := perm1[0]
			flips := 0
			for k > 1 {
				for i, j := 0, k-1; i < j; i, j = i+1, j-1 {
					perm1[i], perm1[j] = perm1[j], perm1[i]
				}
				k = perm1[0]
				flips++
			}
			if flips > maxFlips {
				maxFlips = flips
			}
			if r%2 == 0 {
				checksum += flips
			} else {
				checksum -= flips
			}
		}

		q := 0
		for i := 1; i < r; i++ {
			count[i]++
			if count[i] <= i {
				q = i
				break
			}
			count[i] = 0
		}
		if q == 0 {
			break
		}
		perm1[0], perm1[q] = perm1[q], perm1[0]
		for i, j := 1, q-1; i < j; i, j = i+1, j-1 {
			perm1[i], perm1[j] = perm1[j], perm1[i]
		}
		r = q
	}
	fmt.Printf("%d\n%d\n", checksum, maxFlips)
}
