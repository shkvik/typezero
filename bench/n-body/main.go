package main

import (
	"fmt"
	"math"
)

type Body struct{ x, y, z, vx, vy, vz, mass float64 }

const solarMass = 4 * math.Pi * math.Pi
const daysPerYear = 365.24

func makeBodies() []*Body {
	return []*Body{
		// the Sun
		{0, 0, 0, 0, 0, 0, solarMass},
		// Jupiter
		{
			4.8414314424647209,
			-1.16032004402742839,
			-1.03622044471123109e-1,
			1.66007664274403694e-3 * daysPerYear,
			7.69901118419740425e-3 * daysPerYear,
			-6.90460016972063023e-5 * daysPerYear,
			9.54791938424326609e-4 * solarMass,
		},
		// Saturn
		{
			8.34336671824457987,
			4.12479856412430479,
			-4.03523417114321381e-1,
			-2.76742510726862411e-3 * daysPerYear,
			4.99852801234917238e-3 * daysPerYear,
			2.30417297573763929e-5 * daysPerYear,
			2.85885980666130812e-4 * solarMass,
		},
		// Uranus
		{
			1.2894369562139131e1,
			-1.51111514016986312e1,
			-2.23307578892655734e-1,
			2.96460137564761618e-3 * daysPerYear,
			2.3784717395948095e-3 * daysPerYear,
			-2.96589568540237556e-5 * daysPerYear,
			4.36624404335156298e-5 * solarMass,
		},
		// Neptune
		{
			1.53796971148509165e1,
			-2.59193146099879641e1,
			1.79258772950371181e-1,
			2.68067772490389322e-3 * daysPerYear,
			1.62824170038242295e-3 * daysPerYear,
			-9.5159225451971587e-5 * daysPerYear,
			5.15138902046611451e-5 * solarMass,
		},
	}
}

func offsetMomentum(bodies []*Body) {
	px, py, pz := 0.0, 0.0, 0.0
	for _, b := range bodies {
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	}
	sun := bodies[0]
	sun.vx = -px / solarMass
	sun.vy = -py / solarMass
	sun.vz = -pz / solarMass
}

func advance(bodies []*Body, dt float64) {
	n := len(bodies)
	for i := 0; i < n; i++ {
		bi := bodies[i]
		for j := i + 1; j < n; j++ {
			bj := bodies[j]
			dx := bi.x - bj.x
			dy := bi.y - bj.y
			dz := bi.z - bj.z
			d2 := dx*dx + dy*dy + dz*dz
			mag := dt / (d2 * math.Sqrt(d2))
			bjm := bj.mass * mag
			bi.vx -= dx * bjm
			bi.vy -= dy * bjm
			bi.vz -= dz * bjm
			bim := bi.mass * mag
			bj.vx += dx * bim
			bj.vy += dy * bim
			bj.vz += dz * bim
		}
	}
	for _, b := range bodies {
		b.x += dt * b.vx
		b.y += dt * b.vy
		b.z += dt * b.vz
	}
}

func energy(bodies []*Body) float64 {
	e := 0.0
	n := len(bodies)
	for i := 0; i < n; i++ {
		bi := bodies[i]
		e += 0.5 * bi.mass * (bi.vx*bi.vx + bi.vy*bi.vy + bi.vz*bi.vz)
		for j := i + 1; j < n; j++ {
			bj := bodies[j]
			dx := bi.x - bj.x
			dy := bi.y - bj.y
			dz := bi.z - bj.z
			e -= (bi.mass * bj.mass) / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return e
}

func main() {
	bodies := makeBodies()
	offsetMomentum(bodies)
	fmt.Printf("%.9f\n", energy(bodies))
	for i := 0; i < 5000000; i++ {
		advance(bodies, 0.01)
	}
	fmt.Printf("%.9f\n", energy(bodies))
}
