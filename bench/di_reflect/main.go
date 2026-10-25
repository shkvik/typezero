// di_reflect: the type is known only when the program runs, through package
// reflect: reflect.New makes the object, MethodByName finds the method and
// Call calls it, checking the arguments.
package main

import (
	"fmt"
	"reflect"
)

type Handler struct{ hits int32 }

func (h *Handler) Handle(n int32) int32 {
	h.hits += n
	return h.hits
}

func build(kind reflect.Type) reflect.Value { return reflect.New(kind) }

func invoke(obj reflect.Value, n int32) any {
	method := obj.MethodByName("Handle")
	if !method.IsValid() {
		return nil
	}
	return method.Call([]reflect.Value{reflect.ValueOf(n)})[0].Interface()
}

func main() {
	kinds := []reflect.Type{reflect.TypeOf(Handler{})}
	sum := 0.0
	for _, kind := range kinds {
		for i := int32(0); i < 5000000; i++ {
			h := build(kind)
			invoke(h, i&7)
			if r, ok := invoke(h, 1).(int32); ok {
				sum += float64(r)
			}
		}
	}
	fmt.Println(int64(sum))
}
