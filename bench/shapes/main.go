package main

import "fmt"

type Shape interface {
	area() float64
	perimeter() float64
}

type Circle struct{ r float64 }

func (c *Circle) area() float64      { return 3.141592653589793 * c.r * c.r }
func (c *Circle) perimeter() float64 { return 2 * 3.141592653589793 * c.r }

type Rect struct{ w, h float64 }

func (r *Rect) area() float64      { return r.w * r.h }
func (r *Rect) perimeter() float64 { return 2 * (r.w + r.h) }

type Square struct{ Rect }

func (s *Square) perimeter() float64 { return 4 * s.w }

type Triangle struct{ base, height, side float64 }

func (t *Triangle) area() float64      { return 0.5 * t.base * t.height }
func (t *Triangle) perimeter() float64 { return t.base + t.height + t.side }

func main() {
	shapes := []Shape{}
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
			shapes = append(shapes, &Square{Rect{size, size}})
		default:
			shapes = append(shapes, &Triangle{size, size + 2, size + 3})
		}
	}
	area, perimeter := 0.0, 0.0
	for pass := 0; pass < 20; pass++ {
		for _, s := range shapes {
			area += s.area()
			perimeter += s.perimeter()
		}
	}
	fmt.Printf("%.1f %.1f\n", area, perimeter)
}
