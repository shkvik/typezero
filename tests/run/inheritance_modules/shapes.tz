// The classes and the interface that main.ts extends and implements.
export interface HasArea {
  area(): double
}

export abstract class Shape implements HasArea {
  constructor(public name: string) {}
  abstract area(): double
  describe() {
    return `${this.name} ${this.area().toFixed(1)}`
  }
}

export class Circle extends Shape {
  constructor(public r: double) {
    super("circle")
  }
  area() {
    return Math.PI * this.r ** 2
  }
}
