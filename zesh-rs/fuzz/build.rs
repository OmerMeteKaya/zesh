fn main() {
    cc::Build::new().file("afl_stubs.c").compile("afl_stubs");
}
