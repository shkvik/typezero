package main

import (
	"fmt"
	"math"
)

type Vec struct{ x, y float64 }

func (v *Vec) add(o *Vec) *Vec   { return &Vec{v.x + o.x, v.y + o.y} }
func (v *Vec) sub(o *Vec) *Vec   { return &Vec{v.x - o.x, v.y - o.y} }
func (v *Vec) scale(k float64) *Vec { return &Vec{v.x * k, v.y * k} }
func (v *Vec) lengthSquared() float64 {
	return v.x*v.x + v.y*v.y
}

type Particle struct {
	velocity *Vec
	position *Vec
	mass     float64
}

func newParticle(position *Vec, mass float64) *Particle {
	return &Particle{&Vec{0, 0}, position, mass}
}

func (p *Particle) accelerate(force *Vec, dt float64) {
	p.velocity = p.velocity.add(force.scale(dt / p.mass))
}

func (p *Particle) move(dt float64) {
	p.position = p.position.add(p.velocity.scale(dt))
}

func (p *Particle) energy() float64 {
	return 0.5 * p.mass * p.velocity.lengthSquared()
}

type World struct{ particles []*Particle }

func newWorld(count int32) *World {
	w := &World{}
	for i := int32(0); i < count; i++ {
		x := float64((i*7919)%1000) / 100
		y := float64((i*104729)%1000) / 100
		w.particles = append(w.particles, newParticle(&Vec{x, y}, float64(1+i%5)))
	}
	return w
}

func (w *World) step(dt float64) {
	ps := w.particles
	for i := 0; i < len(ps); i++ {
		a := ps[i]
		force := &Vec{0, 0}
		for j := 0; j < len(ps); j++ {
			if i == j {
				continue
			}
			b := ps[j]
			d := b.position.sub(a.position)
			dist2 := d.lengthSquared() + 0.01
			force = force.add(d.scale((a.mass * b.mass) / (dist2 * math.Sqrt(dist2))))
		}
		a.accelerate(force, dt)
	}
	for _, p := range ps {
		p.move(dt)
	}
}

func (w *World) energy() float64 {
	total := 0.0
	for _, p := range w.particles {
		total += p.energy()
	}
	return total
}

func main() {
	world := newWorld(300)
	for s := 0; s < 120; s++ {
		world.step(0.0001)
	}
	fmt.Printf("%.6f\n", world.energy())
}
