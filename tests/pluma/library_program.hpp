#pragma once

// The Milestone 9.2a "done when" program (DESIGN.md 6): one script that uses
// every collection, List / str / number method, conversion, Result with `?`,
// optional helper, property accessor and method value. library_tests.cpp
// checks its output under the VM; aot_tests.cpp checks that AOT-to-C prints
// exactly the same.

namespace test {

inline constexpr const char* library_program = R"PLM(
// ---- Map: insertion order, index, compound assignment, iteration
var stock = {"apple": 3, "pear": 1}
stock["fig"] = 7
stock["apple"] += 10
stock.remove("pear")
stock["pear"] = 2
print(stock)
for k, v in stock { print("{k} -> {v}") }
for k in stock { print(k) }
print(stock.len())
print(stock.has("fig"))
print(stock.get("kiwi") ?? -1)
print(stock.get_or("kiwi", 0))
print(stock.keys())
print(stock.values())
var by_id = Map<int, str>()
for i in 0..40 { by_id[i * 7 % 40] = "n{i}" }
for i in 0..30 { by_id.remove(i) }
print(by_id.len())
print(by_id.keys())

// ---- Set
var seen: Set<str> = ["b", "a", "b", "c"]
print(seen)
print(seen.add("a"))
print(seen.add("d"))
print(seen.contains("c"))
var other: Set<str> = ["c", "d", "e"]
print(seen.union(other))
print(seen.intersection(other))
print(seen.difference(other))
for x in seen { print(x) }

// ---- [T; N]: a value type
var grid: [int; 4]
grid[2] = 5
var copy = grid
copy[0] = 9
print(grid)
print(copy)
print(grid == [0, 0, 5, 0])
print(grid.len())
var lit: [float; 3] = [1.5, 0.5, 2.5]
lit.sort()
print(lit)
struct Cell {
    var marks: [int; 2]
}
var c1 = Cell {}
var c2 = c1
c2.marks[1] = 4
print(c1.marks)
print(c2.marks)

// ---- StrBuilder
var sb = StrBuilder()
for i in 0..3 { sb.append(i).append(";") }
print(sb.to_str())
print(sb.len())

// ---- Deque
var dq: Deque<int> = [2, 3]
dq.push_front(1)
dq.push_back(4)
print(dq)
print(dq.pop_front())
print(dq.pop_back())
print(dq.front())
print(dq.back())
dq[0] = 20
for i, x in dq { print("{i}: {x}") }

// ---- PriorityQueue (min-heap; equal priorities come out in push order)
struct Job {
    var pri: int
    var name: str
}
var jobs = PriorityQueue<Job>((a, b) => a.pri < b.pri)
jobs.push(Job { pri = 2, name = "b1" })
jobs.push(Job { pri = 1, name = "a" })
jobs.push(Job { pri = 2, name = "b2" })
jobs.push(Job { pri = 0, name = "z" })
print(jobs.peek().name)
while !jobs.is_empty() { print(jobs.pop().name) }
var nums = PriorityQueue<int>()
for x in [5, 1, 4] { nums.push(x) }
print(nums.to_list())

// ---- List methods
var xs = [5, 3, 8, 1, 9, 2]
xs.sort()
print(xs)
xs.sort((a, b) => a > b)
print(xs)
var words = ["pear", "fig", "banana", "kiwi"]
words.sort_by(w => w.len())
print(words)
xs.reverse()
print(xs)
print(xs.index_of(8))
print(xs.index_of(42))
print(xs.slice(1, 3))
print(xs.slice(4))
xs.extend([7, 7])
xs.swap(0, 1)
print(xs)
print(xs.first() ?? -1)
print(xs.last() ?? -1)
print(List<int>().first() ?? -1)
print(xs.map(x => x * 10))
print(xs.map(x => "<{x}>"))
print(xs.filter(x => x % 2 == 1))
print(xs.reduce(0, (acc, x) => acc + x))
print(xs.any(x => x > 8))
print(xs.all(x => x > 0))
print(xs.count(x => x == 7))
print(xs.unique())
print(xs.min() ?? 0)
print(xs.max() ?? 0)
print(xs.sum())
print([0.5, 1.25].sum())
print(xs.zip(words, (n, w) => "{w}{n}"))
var deck = [1, 2, 3, 4, 5, 6, 7, 8]
deck.shuffle()
print(deck)
print([3.0, 0.0 / 0.0, -1.0].min() ?? 0.0)

// ---- str methods
var line = "  Hello, World  "
print("[{line.trim()}] [{line.trim_start()}] [{line.trim_end()}]")
print("a,b,,c".split(","))
print("-".join(["x", "y", "z"]))
print("cat hat".replace("at", "og"))
print("MiXeD".to_upper() + " " + "MiXeD".to_lower())
print("banana".find("an"))
print("banana".find("an", 2))
print("banana".find("x"))
print("héllo".substr(1, 2))
print("abc".byte_at(1))
print("héllo".chars())
print("héllo".char_count())
print("héllo".len())
print("ab".repeat(3))
print("7".pad_left(3, "0") + "|" + "ab".pad_right(4) + "|")
print("one\ntwo\r\nthree\n".lines())

// ---- numbers and conversions
print(255.to_hex())
print((-26).to_radix(2))
print(3.14159.to_fixed(3))
print(2.5.to_fixed(0))
print(parse_int("123") ?? 0)
print(parse_int("-7f", 16) ?? 0)
print(parse_int("12x") ?? -1)
print(parse_int("99999999999") ?? -1)
print("0.25".parse_float() ?? -1.0)
print(parse_float("abc") ?? -1.0)
var opt: int? = parse_int("41")
print(opt.unwrap() + 1)
print(opt.is_some())
print(parse_int("no").is_none())

// ---- Result and ?
fn parse_pair(s: str) -> Result<int, str> {
    var parts = s.split(":")
    if parts.len() != 2 { return Err("expected a:b, got '{s}'") }
    var a = parts[0].parse_int()
    var b = parts[1].parse_int()
    if a? x {
        if b? y { return Ok(x * y) }
    }
    return Err("not numbers: '{s}'")
}
fn total(items: List<str>) -> Result<int, str> {
    var sum = 0
    for it in items { sum += parse_pair(it)? }
    return Ok(sum)
}
fn first_even(xs: List<int>) -> int? {
    for x in xs { if x % 2 == 0 { return x } }
    return null
}
fn doubled_even(xs: List<int>) -> int? {
    var e = first_even(xs)?
    return e * 2
}
print(total(["2:3", "4:5"]))
print(total(["2:3", "4"]))
print(total(["2:x"]))
match total(["1:1"]) {
    Ok(v) => print("ok {v}")
    Err(e) => print("err {e}")
}
var r = parse_pair("6:7")
print(r.is_ok())
print(r.unwrap())
print(parse_pair("x").unwrap_or(-1))
print(parse_pair("x").unwrap_err())
print(parse_pair("1:2").ok() ?? 0)
print(doubled_even([1, 3, 4]) ?? -1)
print(doubled_even([1, 3]) ?? -1)
if r is Ok(v) { print("bound {v}") }

// ---- property accessors and method values
class Gauge {
    var max = 10
    var level = 0 {
        set(v) {
            level = clamp(v, 0, max)
            writes += 1
        }
    }
    var writes = 0
    var ratio: float {
        get { return level as float / max as float }
    }
    var text: str {
        get { return "{level}/{max}" }
        set(v) { level = v.len() }
    }
    fn bump(n: int) { level += n }
    fn times(k: int) -> int { return level * k }
}
var g = Gauge()
g.level = 25
print(g.level)
g.level -= 3
print(g.text)
print(g.ratio)
g.text = "abc"
print(g.text)
print(g.writes)
var bump = g.bump
bump(4)
var times: fn(int) -> int = g.times
print(times(3))
var ops = [g.times, fn(x: int) -> int { return -x }]
print(ops.map(f => f(2)))
)PLM";

inline constexpr const char* library_expected = R"OUT({"apple": 13, "fig": 7, "pear": 2}
apple -> 13
fig -> 7
pear -> 2
apple
fig
pear
3
true
-1
0
["apple", "fig", "pear"]
[13, 7, 2]
10
[35, 30, 37, 32, 39, 34, 36, 31, 38, 33]
Set["b", "a", "c"]
false
true
true
Set["b", "a", "c", "d", "e"]
Set["c", "d"]
Set["b", "a"]
b
a
c
d
[0, 0, 5, 0]
[9, 0, 5, 0]
true
4
[0.5, 1.5, 2.5]
[0, 0]
[0, 4]
0;1;2;
6
Deque[1, 2, 3, 4]
1
4
2
3
0: 20
1: 3
z
z
a
b1
b2
[1, 4, 5]
[1, 2, 3, 5, 8, 9]
[9, 8, 5, 3, 2, 1]
["fig", "pear", "kiwi", "banana"]
[1, 2, 3, 5, 8, 9]
4
-1
[2, 3]
[8, 9]
[2, 1, 3, 5, 8, 9, 7, 7]
2
7
-1
[20, 10, 30, 50, 80, 90, 70, 70]
["<2>", "<1>", "<3>", "<5>", "<8>", "<9>", "<7>", "<7>"]
[1, 3, 5, 9, 7, 7]
42
true
true
2
[2, 1, 3, 5, 8, 9, 7]
1
9
42
1.75
["fig2", "pear1", "kiwi3", "banana5"]
[8, 3, 2, 7, 4, 5, 1, 6]
-1.0
[Hello, World] [Hello, World  ] [  Hello, World]
["a", "b", "", "c"]
x-y-z
cog hog
MIXED mixed
1
3
-1
é
98
["h", "é", "l", "l", "o"]
5
6
ababab
007|ab  |
["one", "two", "three"]
ff
-11010
3.142
2
123
-127
-1
-1
0.25
-1.0
42
true
true
Ok(26)
Err("expected a:b, got '4'")
Err("not numbers: '2:x'")
ok 1
true
42
-1
expected a:b, got 'x'
2
8
-1
bound 42
10
7/10
0.7
3/10
3
21
[14, -2]
)OUT";

} // namespace test
