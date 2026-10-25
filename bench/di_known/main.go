// di_known: the classes of the container are known when compiling, so the
// generic make and invoke are direct construction and a direct call.
package main

import "fmt"

type Handler struct{ hits int32 }

func (h *Handler) Handle(n int32) int32 {
	h.hits += n
	return h.hits
}

func build[T any](ctor func() *T) *T { return ctor() }

func invoke(h *Handler, n int32) any { return h.Handle(n) }

func main() {
	sum := 0.0
	for i := int32(0); i < 5000000; i++ {
		h := build(func() *Handler { return &Handler{} })
		invoke(h, i&7)
		if r, ok := invoke(h, 1).(int32); ok {
			sum += float64(r)
		}
	}
	fmt.Println(int64(sum))
}
