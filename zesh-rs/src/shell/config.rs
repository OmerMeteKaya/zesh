// Config loading

pub fn config_load() {
    // Load shell configuration
    // For now, just set up basic environment
    unsafe {
        libc::srand(libc::time(std::ptr::null_mut()) as u32);
    }
}
