package main

import "fmt"

type Shape interface{ isShape() }

type Circle struct{ r float64 }
type Rect struct{ w, h float64 }
type Square struct{ side float64 }
type Triangle struct{ base, height, side float64 }

func (*Circle) isShape()   {}
func (*Rect) isShape()     {}
func (*Square) isShape()   {}
func (*Triangle) isShape() {}

func area(s Shape) float64 {
	switch s := s.(type) {
	case *Circle:
		return 3.141592653589793 * s.r * s.r
	case *Rect:
		return s.w * s.h
	case *Square:
		return s.side * s.side
	case *Triangle:
		return 0.5 * s.base * s.height
	}
	return 0
}

func perimeter(s Shape) float64 {
	switch s := s.(type) {
	case *Circle:
		return 2 * 3.141592653589793 * s.r
	case *Rect:
		return 2 * (s.w + s.h)
	case *Square:
		return 4 * s.side
	case *Triangle:
		return s.base + s.height + s.side
	}
	return 0
}

func main() {
	shapes := []Shape{}
	tokens := []any{}
	seed := int32(12345)
	for i := 0; i < 1000000; i++ {
		seed = (seed*1103515245 + 12345) & 0x7fffffff
		size := 1 + float64(seed%100)/10
		kind := (seed >> 8) & 3
		switch kind {
		case 0:
			shapes = append(shapes, &Circle{size})
		case 1:
			shapes = append(shapes, &Rect{size, size + 1})
		case 2:
			shapes = append(shapes, &Square{size})
		default:
			shapes = append(shapes, &Triangle{size, size + 2, size + 3})
		}
		if seed&1 == 0 {
			tokens = append(tokens, seed%1000)
		} else if kind == 0 {
			tokens = append(tokens, "a")
		} else if kind == 1 {
			tokens = append(tokens, "bb")
		} else {
			tokens = append(tokens, "ccc")
		}
	}
	a, p, t := 0.0, 0.0, int64(0)
	for pass := 0; pass < 20; pass++ {
		for _, s := range shapes {
			a += area(s)
			p += perimeter(s)
		}
		for _, x := range tokens {
			switch x := x.(type) {
			case string:
				t += int64(len(x))
			case int32:
				t += int64(x)
			}
		}
	}
	fmt.Printf("%.1f %.1f %d\n", a, p, t)
}
