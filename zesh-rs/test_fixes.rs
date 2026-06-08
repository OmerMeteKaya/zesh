use zesh_rs::shell::expand::eval_arith_simple;
use zesh_rs::shell::vars::VarStore;
use zesh_rs::shell::expand::expand_word;

fn main() {
    println!("Testing Bug 1 - Shift overflow:");

    // Test 1: Large left shift
    match eval_arith_simple("$(( 1 << 999 ))") {
        Ok(v) => println!("  1 << 999 = {} (should be clamped to 1 << 63)", v),
        Err(e) => println!("  Error: {}", e),
    }

    // Test 2: Negative shift
    match eval_arith_simple("$(( 1 << -5 ))") {
        Ok(v) => println!("  1 << -5 = {} (should be 0)", v),
        Err(e) => println!("  Error: {}", e),
    }

    // Test 3: Right shift overflow
    match eval_arith_simple("$(( 64 >> 100 ))") {
        Ok(v) => println!("  64 >> 100 = {} (should be 0)", v),
        Err(e) => println!("  Error: {}", e),
    }

    println!("\nTesting Bug 2 - Substring expansion:");

    let vars = VarStore::new();

    // Test 4: Negative offset
    let result = expand_word("${var:-2:3}", false, &vars, "");
    println!("  ${{var:-2:3}} = {:?}", result);

    // Test 5: Large positive offset
    let result = expand_word("${var:99999999:99999999}", false, &vars, "");
    println!("  ${{var:99999999:99999999}} = {:?}", result);

    // Test 6: Variable with negative substring length
    let result = expand_word("${var:0:-5}", false, &vars, "");
    println!("  ${{var:0:-5}} = {:?}", result);

    println!("\nAll tests passed without panics!");
}
