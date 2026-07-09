// example.js
// Run: novac port example.js
// to get a novac ported version
// basically showcasing js2novac capabilities
// note: this comment will get pasted as is
const greeting = "Hello, World!";
let count = 0;

function add(a, b) {
    return a + b;
}

function greet(name) {
    const msg = `Hello, ${name}!`;
    console.log(msg);
}

const double = x => x * 2;

class Animal {
    constructor(n, s) {
        this.name = n;
        this.sound = s;
    }

    *speak() {
        console.log(`${this.name} says ${this.sound}`);
    }
}

const dog = new Animal("Rex", "woof");
dog.speak();

for (let i = 0; i < 5; i++) {
    count += 1;
}

const nums = [1, 2, 3, 4, 5];
const doubled = nums.map(n => n * 2);
const evens = nums.filter(n => n % 2 === 0);

console.log(add(3, 4));
console.log(doubled);
console.log(evens);
greet("Nova");