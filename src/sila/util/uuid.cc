// uuid.cc — RFC 4122 v4 UUID generation
#include "uuid.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace sila2::util {

std::string generateUuid() {
    // thread_local: each thread seeds its own engine once, on first call,
    // instead of contending on a shared lock just to draw random bits.
    // random_device is only used as a one-time seed, not per-call, since it
    // can be slow and some platforms limit how often it may be read.
    /*  thread_local:         스레드마다 독립된 copy 생성, 스레드 간 동기화(Lock/Mutex)가 필요 없어 멀티스레드 환경에서 빠르고 안전
        std::mt19937_64:      64비트 메르센 트위스터 난수 엔진.
        std::random_device{}: OS 제공 하드웨어/엔트로피 기반 난수 소스 임시 객체 생성 후 호출 '()'
    */
    thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uint64_t hi = engine();
    std::uint64_t lo = engine();

    // RFC 4122 §4.4 UUID v4 layout: hi holds time_low / time_mid / time_hi_and_version,
    // lo holds clock_seq_hi_and_reserved / clock_seq_low / node.
    // Force the version nibble (bits 12-15 of hi) to 0100,
    // and the variant bits (top 2 bits of lo) to 10, leaving every other bit random.
    /*  단순 128비트는 표준 UUID 아님.
        RFC 4122을 준수하기 위해 특정 비트 강제 고정
        0xF = 0d15 = 0b1111, ULL: "Unsigned Long Long" 타입 접미사
        &: AND 연산자, | OR 연산자
        hi: 13번째 16진수 자리는 버전 비트 위치. v4이므로 항상 0x4여야함. AND 연산으로 해당 비트 0 -> OR 연산으로 해당 비트 0x4
        lo: 1번째 16진수의 첫 2비트는 UUID Variant 비트. 항상 0d10 여야함. AND 연산으로 첫 2 비트 0d00 -> OR 연산으로 해당 비트 0x10
    */
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    /*  128비트 비트 데이터를 하이픈(-)이 포함된 표준 36자 문자열(널 문자 포함 37바이트 버퍼)로 변환
            hi: 64비트를 8자리-4자리-4자리 Hex로 분할
            lo: 64비트를 4자리-12자리 Hex로 분할
        >> (right Shift): 비트를 지정한 칸수만큼 오른쪽으로 밀어냄. 밀려난 비트는 사라지고, 상위 비트가 아래로 내려옴.
            static_cast<unsigned>(hi >> 32): 상위 32비트가 최하위 자리로 내려오며, 이후 unsigned 캐스팅
    */
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(hi >> 32),
        static_cast<unsigned>((hi >> 16) & 0xFFFF),
        static_cast<unsigned>(hi & 0xFFFF),
        static_cast<unsigned>(lo >> 48),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

}  // namespace sila2::util
