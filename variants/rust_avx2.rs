// HighLoad.fun — parse_integers (VARIANT: rust_avx2)
// Direct Rust port of avx2_parse_quad champion.
// Hypothesis: rustc/LLVM may schedule SIMD loads and scalar chains differently
// from clang++, potentially yielding better register assignment or instruction
// interleaving on the cnt==6 hot path.
//
// Uses the same algorithm: AVX2 64-byte nl_mask64, parse_quad (256-bit) +
// parse_pair (128-bit) + parse_num (SWAR). Handles cnt==4,5,6,7 fast paths.
// ARM fallback: scalar SWAR solver (same as champion's #else branch).
//
// Build: rustc -C opt-level=3 -C target-cpu=native variants/rust_avx2.rs -o /tmp/...
#![allow(non_snake_case)]

use std::arch::x86_64::*;
use std::mem::MaybeUninit;

// --- Raw libc bindings (no extern crate) ---
#[repr(C)]
struct Stat {
    st_dev: u64, st_ino: u64, st_nlink: u64,
    st_mode: u32, st_uid: u32, st_gid: u32, _pad0: u32,
    st_rdev: u64, st_size: i64, st_blksize: i64, st_blocks: i64,
    _timefields: [i64; 6], _unused: [i64; 3],
}
extern "C" {
    fn fstat(fd: i32, buf: *mut Stat) -> i32;
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    fn madvise(addr: *mut u8, len: usize, advice: i32) -> i32;
    fn read(fd: i32, buf: *mut u8, count: usize) -> isize;
    fn write(fd: i32, buf: *const u8, count: usize) -> isize;
}
const PROT_READ: i32 = 1;
const MAP_PRIVATE: i32 = 2;
const MAP_POPULATE: i32 = 0x8000;
const MADV_SEQUENTIAL: i32 = 2;
const MAP_FAILED: *mut u8 = !0usize as *mut u8;

// --- SWAR parse_8: parse 8 ASCII digits packed in a u64 (LE) ---
#[inline(always)]
fn parse_8(chunk: u64) -> u32 {
    let lo = (chunk & 0x0f000f000f000f00u64) >> 8;
    let hi = (chunk & 0x000f000f000f000fu64).wrapping_mul(10);
    let chunk = lo.wrapping_add(hi);
    let lo = (chunk & 0x00ff000000ff0000u64) >> 16;
    let hi = (chunk & 0x000000ff000000ffu64).wrapping_mul(100);
    let chunk = lo.wrapping_add(hi);
    let lo = (chunk & 0x0000ffff00000000u64) >> 32;
    let hi = (chunk & 0x000000000000ffffu64).wrapping_mul(10000);
    lo.wrapping_add(hi) as u32
}

#[inline(always)]
unsafe fn load_u64(p: *const u8) -> u64 {
    let mut v = MaybeUninit::<u64>::uninit();
    std::ptr::copy_nonoverlapping(p, v.as_mut_ptr() as *mut u8, 8);
    v.assume_init()
}

#[inline(always)]
unsafe fn parse_num(p: *const u8, len: usize) -> u64 {
    if len <= 8 {
        let w0 = load_u64(p);
        return parse_8(w0 << ((8 - len) * 8)) as u64;
    }
    let chunk = load_u64(p.add(len - 8));
    let d0 = (*p as u64).wrapping_sub(b'0' as u64);
    let is10 = (len == 10) as u64;
    let d1 = (*p.add(1) as u64).wrapping_sub(b'0' as u64);
    let high = d0.wrapping_mul(1 + 9 * is10).wrapping_add(is10 * d1);
    high.wrapping_mul(100_000_000u64).wrapping_add(parse_8(chunk) as u64)
}

#[inline(always)]
unsafe fn scalar_tail(mut p: *const u8, end: *const u8, mut sum: u64) -> u64 {
    let mut v = 0u64;
    while p < end {
        let d = (*p as u32).wrapping_sub('0' as u32);
        if d <= 9 { v = v.wrapping_mul(10).wrapping_add(d as u64); }
        else { sum = sum.wrapping_add(v); v = 0; }
        p = p.add(1);
    }
    sum.wrapping_add(v)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,ssse3,sse4.1")]
unsafe fn parse_pair(
    p_a: *const u8, len_a: usize,
    p_b: *const u8, len_b: usize,
) -> u64 {
    if len_a < 8 || len_b < 8 {
        return parse_num(p_a, len_a) + parse_num(p_b, len_b);
    }
    let ca = _mm_loadl_epi64(p_a.add(len_a - 8) as *const __m128i);
    let cb = _mm_loadl_epi64(p_b.add(len_b - 8) as *const __m128i);
    let chunks = _mm_unpacklo_epi64(ca, cb);
    let chunks = _mm_sub_epi8(chunks, _mm_set1_epi8(b'0' as i8));
    let w1 = _mm_set_epi8(1,10,1,10,1,10,1,10, 1,10,1,10,1,10,1,10);
    let l1 = _mm_maddubs_epi16(chunks, w1);
    let w2 = _mm_set_epi16(1,100,1,100, 1,100,1,100);
    let l2 = _mm_madd_epi16(l1, w2);
    let w3 = _mm_set_epi32(1, 10000, 1, 10000);
    let l3 = _mm_mullo_epi32(l2, w3);
    let l3s = _mm_shuffle_epi32(l3, 0xB1);
    let l4 = _mm_add_epi32(l3, l3s);
    let a8 = _mm_cvtsi128_si32(l4) as u32;
    let b8 = _mm_extract_epi32(l4, 2) as u32;

    let a0 = (*p_a as u64).wrapping_sub(b'0' as u64);
    let b0 = (*p_b as u64).wrapping_sub(b'0' as u64);
    let a_is9 = (len_a == 9) as u64; let a_is10 = (len_a == 10) as u64;
    let b_is9 = (len_b == 9) as u64; let b_is10 = (len_b == 10) as u64;
    let a1 = (*p_a.add(1) as u64).wrapping_sub(b'0' as u64);
    let b1 = (*p_b.add(1) as u64).wrapping_sub(b'0' as u64);
    let ah = a0*a_is9 + (a0*10+a1)*a_is10;
    let bh = b0*b_is9 + (b0*10+b1)*b_is10;
    (ah*100_000_000+a8 as u64) + (bh*100_000_000+b8 as u64)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,ssse3,sse4.1")]
unsafe fn parse_quad(
    p_a: *const u8, len_a: usize,
    p_b: *const u8, len_b: usize,
    p_c: *const u8, len_c: usize,
    p_d: *const u8, len_d: usize,
) -> u64 {
    if len_a < 8 || len_b < 8 || len_c < 8 || len_d < 8 {
        return parse_pair(p_a,len_a,p_b,len_b) + parse_pair(p_c,len_c,p_d,len_d);
    }
    let ca = _mm_loadl_epi64(p_a.add(len_a-8) as *const __m128i);
    let cb = _mm_loadl_epi64(p_b.add(len_b-8) as *const __m128i);
    let lo128 = _mm_unpacklo_epi64(ca, cb);
    let cc = _mm_loadl_epi64(p_c.add(len_c-8) as *const __m128i);
    let cd = _mm_loadl_epi64(p_d.add(len_d-8) as *const __m128i);
    let hi128 = _mm_unpacklo_epi64(cc, cd);
    let chunks = _mm256_set_m128i(hi128, lo128);
    let chunks = _mm256_sub_epi8(chunks, _mm256_set1_epi8(b'0' as i8));

    let seed = _mm_set_epi8(1,10,1,10,1,10,1,10, 1,10,1,10,1,10,1,10);
    let w1 = _mm256_broadcastsi128_si256(seed);
    let l1 = _mm256_maddubs_epi16(chunks, w1);
    let seed2 = _mm_set_epi16(1,100,1,100, 1,100,1,100);
    let w2 = _mm256_broadcastsi128_si256(seed2);
    let l2 = _mm256_madd_epi16(l1, w2);
    let w3 = _mm256_set_epi32(1,10000,1,10000, 1,10000,1,10000);
    let l3 = _mm256_mullo_epi32(l2, w3);
    let l3s = _mm256_shuffle_epi32(l3, 0xB1);
    let l4 = _mm256_add_epi32(l3, l3s);
    let lo4 = _mm256_castsi256_si128(l4);
    let hi4 = _mm256_extracti128_si256(l4, 1);
    let a8 = _mm_cvtsi128_si32(lo4) as u32;
    let b8 = _mm_extract_epi32(lo4, 2) as u32;
    let c8 = _mm_cvtsi128_si32(hi4) as u32;
    let d8 = _mm_extract_epi32(hi4, 2) as u32;

    let a0=(*p_a as u64)-b'0' as u64; let b0=(*p_b as u64)-b'0' as u64;
    let c0=(*p_c as u64)-b'0' as u64; let d0=(*p_d as u64)-b'0' as u64;
    let a_is9=(len_a==9) as u64; let a_is10=(len_a==10) as u64;
    let b_is9=(len_b==9) as u64; let b_is10=(len_b==10) as u64;
    let c_is9=(len_c==9) as u64; let c_is10=(len_c==10) as u64;
    let d_is9=(len_d==9) as u64; let d_is10=(len_d==10) as u64;
    let a1=(*p_a.add(1) as u64)-b'0' as u64;
    let b1=(*p_b.add(1) as u64)-b'0' as u64;
    let c1=(*p_c.add(1) as u64)-b'0' as u64;
    let d1=(*p_d.add(1) as u64)-b'0' as u64;
    let ah=a0*a_is9+(a0*10+a1)*a_is10; let bh=b0*b_is9+(b0*10+b1)*b_is10;
    let ch=c0*c_is9+(c0*10+c1)*c_is10; let dh=d0*d_is9+(d0*10+d1)*d_is10;
    (ah*100_000_000+a8 as u64)+(bh*100_000_000+b8 as u64)
   +(ch*100_000_000+c8 as u64)+(dh*100_000_000+d8 as u64)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn nl_mask64(p: *const u8) -> u64 {
    let v0 = _mm256_loadu_si256(p as *const __m256i);
    let v1 = _mm256_loadu_si256(p.add(32) as *const __m256i);
    let nl = _mm256_set1_epi8(b'\n' as i8);
    let m0 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, nl)) as u32;
    let m1 = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, nl)) as u32;
    (m0 as u64) | ((m1 as u64) << 32)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,ssse3,sse4.1")]
unsafe fn solve_avx2(data: *const u8, size: usize) -> u64 {
    let end = data.add(size);
    let mut sum = 0u64;
    if size < 128 { return scalar_tail(data, end, sum); }
    let mut base = data;
    let safe_end = end.sub(96);
    let mut p = data;

    while p < safe_end {
        let m = nl_mask64(p);
        let cnt = m.count_ones() as i32;

        if cnt == 6 {
            let mut mm = m;
            let n0 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n1 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n2 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n3 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n4 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n5 = mm.trailing_zeros() as usize;
            let nl0=p.add(n0); let nl1=p.add(n1); let nl2=p.add(n2);
            let nl3=p.add(n3); let nl4=p.add(n4); let nl5=p.add(n5);
            sum += parse_quad(base,   nl0.offset_from(base) as usize,
                              nl0.add(1), nl1.offset_from(nl0) as usize - 1,
                              nl1.add(1), nl2.offset_from(nl1) as usize - 1,
                              nl2.add(1), nl3.offset_from(nl2) as usize - 1)
                 + parse_pair(nl3.add(1), nl4.offset_from(nl3) as usize - 1,
                              nl4.add(1), nl5.offset_from(nl4) as usize - 1);
            base = nl5.add(1);
        } else if cnt == 5 {
            let mut mm = m;
            let n0 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n1 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n2 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n3 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n4 = mm.trailing_zeros() as usize;
            let nl0=p.add(n0); let nl1=p.add(n1); let nl2=p.add(n2);
            let nl3=p.add(n3); let nl4=p.add(n4);
            sum += parse_quad(base,   nl0.offset_from(base) as usize,
                              nl0.add(1), nl1.offset_from(nl0) as usize - 1,
                              nl1.add(1), nl2.offset_from(nl1) as usize - 1,
                              nl2.add(1), nl3.offset_from(nl2) as usize - 1)
                 + parse_num(nl3.add(1), nl4.offset_from(nl3) as usize - 1);
            base = nl4.add(1);
        } else if cnt == 7 {
            let mut mm = m;
            let n0 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n1 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n2 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n3 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n4 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n5 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n6 = mm.trailing_zeros() as usize;
            let nl0=p.add(n0); let nl1=p.add(n1); let nl2=p.add(n2);
            let nl3=p.add(n3); let nl4=p.add(n4); let nl5=p.add(n5); let nl6=p.add(n6);
            sum += parse_quad(base,   nl0.offset_from(base) as usize,
                              nl0.add(1), nl1.offset_from(nl0) as usize - 1,
                              nl1.add(1), nl2.offset_from(nl1) as usize - 1,
                              nl2.add(1), nl3.offset_from(nl2) as usize - 1)
                 + parse_pair(nl3.add(1), nl4.offset_from(nl3) as usize - 1,
                              nl4.add(1), nl5.offset_from(nl4) as usize - 1)
                 + parse_num(nl5.add(1), nl6.offset_from(nl5) as usize - 1);
            base = nl6.add(1);
        } else if cnt == 4 {
            let mut mm = m;
            let n0 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n1 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n2 = mm.trailing_zeros() as usize; mm &= mm-1;
            let n3 = mm.trailing_zeros() as usize;
            let nl0=p.add(n0); let nl1=p.add(n1); let nl2=p.add(n2); let nl3=p.add(n3);
            sum += parse_quad(base,   nl0.offset_from(base) as usize,
                              nl0.add(1), nl1.offset_from(nl0) as usize - 1,
                              nl1.add(1), nl2.offset_from(nl1) as usize - 1,
                              nl2.add(1), nl3.offset_from(nl2) as usize - 1);
            base = nl3.add(1);
        } else {
            let mut mm = m;
            while mm != 0 {
                let nlpos = mm.trailing_zeros() as usize;
                let nlp = p.add(nlpos);
                let len = nlp.offset_from(base) as usize;
                if len > 0 { sum += parse_num(base, len); }
                base = nlp.add(1);
                mm &= mm - 1;
            }
        }
        p = p.add(64);
    }
    scalar_tail(base, end, sum)
}

fn solve_scalar(data: &[u8]) -> u64 {
    let mut sum = 0u64;
    let mut v = 0u64;
    for &b in data {
        let d = b.wrapping_sub(b'0') as u32;
        if d <= 9 { v = v.wrapping_mul(10).wrapping_add(d as u64); }
        else { sum = sum.wrapping_add(v); v = 0; }
    }
    sum.wrapping_add(v)
}

fn print_u64(n: u64) {
    let mut buf = [0u8; 21];
    let mut i = 20usize;
    buf[i] = b'\n';
    let mut x = n;
    if x == 0 {
        i -= 1; buf[i] = b'0';
    } else {
        while x > 0 {
            i -= 1; buf[i] = b'0' + (x % 10) as u8; x /= 10;
        }
    }
    unsafe { write(1, buf.as_ptr().add(i), 21 - i); }
}

fn main() {
    unsafe {
        let mut st = MaybeUninit::<Stat>::uninit();
        if fstat(0, st.as_mut_ptr()) != 0 { return; }
        let size = st.assume_init().st_size as usize;

        if size > 0 {
            let data = mmap(std::ptr::null_mut(), size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, 0, 0);
            if data != MAP_FAILED {
                madvise(data, size, MADV_SEQUENTIAL);
                #[cfg(target_arch = "x86_64")]
                {
                    if is_x86_feature_detected!("avx2") && is_x86_feature_detected!("ssse3") {
                        let sum = solve_avx2(data, size);
                        print_u64(sum);
                        return;
                    }
                }
                let slice = std::slice::from_raw_parts(data, size);
                print_u64(solve_scalar(slice));
                return;
            }
        }

        // Fallback: read from stdin
        let mut buf = vec![0u8; 1 << 26];
        let mut total = 0usize;
        loop {
            let n = read(0, buf.as_mut_ptr().add(total), buf.len() - total);
            if n <= 0 { break; }
            total += n as usize;
            if total >= buf.len() { break; }
        }
        let slice = &buf[..total];
        #[cfg(target_arch = "x86_64")]
        if is_x86_feature_detected!("avx2") && is_x86_feature_detected!("ssse3") {
            let sum = solve_avx2(slice.as_ptr(), total);
            print_u64(sum);
            return;
        }
        print_u64(solve_scalar(slice));
    }
}
