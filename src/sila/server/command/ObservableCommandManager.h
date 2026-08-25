// ObservableCommandManager.h — UUID → ObservableCommandExecution map (architecture.md §3.3)
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace sila2 {

class ObservableCommandExecution;

/// Manages the set of active Observable Command executions (architecture.md §3.3).
/// Owns the UUID → execution map, generates UUIDs, and provides lifecycle
/// management (lookup, expiration sweep, bulk cancellation for shutdown).
///
/// Thread-safe: all public methods lock an internal mutex.
class ObservableCommandManager {
public:
    ObservableCommandManager();
    // Defined in the .cc: ObservableCommandExecution is only forward-declared
    // above, and unique_ptr's deleter needs the complete type at the point
    // where commands_ is destroyed.
    ~ObservableCommandManager();

    /// Create and register a new command execution with an auto-generated UUID.
    /// @param lifetime Duration after finish before the execution is eligible for GC.
    ///                 Zero means never expires.
    /// @return Reference to the newly created execution. Valid until removeExpired()
    ///         or the manager is destroyed.
    ObservableCommandExecution& addCommand(
        std::chrono::seconds lifetime = std::chrono::seconds{0});

    /// Look up a command execution by UUID.
    /// @throws sila2::error::FrameworkError with InvalidCommandExecutionUuid if not found.
    ObservableCommandExecution& getCommand(const std::string& uuid);

    /// Remove all finished executions whose lifetime has elapsed.
    /// @return Number of executions removed.
    std::size_t removeExpired();

    /// Request interruption on all executions (for server shutdown).
    void interruptAll();

    /// Start a background thread that calls removeExpired() every @p interval.
    /// No-op if auto-GC is already running.
    /// @param interval Sweep period.
    void startAutoGC(std::chrono::seconds interval);

    /// Stop the background GC thread. No-op if not running.
    /// Also called by the destructor.
    void stopAutoGC();

    /// @return true if the auto-GC background thread is running.
    [[nodiscard("caller expects the auto-GC status")]]
    bool isAutoGCRunning() const;

    /// @return Current number of tracked executions.
    [[nodiscard("caller expects the execution count")]]
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    /*  std::unordered_map:
    map의 이진 트리 구조가 아닌 hash table로 pair 정렬. 비결정적 순회 순서.
    조회가 빠르고, 순서에 의존하는 코드가 없으면 이득.
        key:    uuid std::string,
        value:  commands_ *ObservableCommandExecution

    unique_ptr가 ObservableCommandExecution을 해제(delete)하는 경우:
      1. 매니저 소멸 → commands_ 맵 소멸 → 전체 엔트리 해제.
      2. 맵에서 erase (예: removeExpired()).
      3. reset() 호출 (ptr.reset() 시 기존 객체 해제하고 nullptr 상태가 됨.
                      ptr.reset(new_obj) 시 기존 객체 해제하고 new_obj에 대한 ptr 생성).
    */
    std::unordered_map<std::string, std::unique_ptr<ObservableCommandExecution>> commands_;
    
    /*  std::atomic: 락 없이 여러 스레드가 동시에 읽고 써도 데이터 레이스가 발생하지 않는 boolean.
        std::thread: OS 스레드 하나를 소유하는 RAII 핸들.
                     소멸 전에 반드시 join() 또는 detach(). 안 하면 std::terminate.
        std::mutex:  상호 배제 잠금(mutual exclusion lock). 
                     lock()/unlock()으로 임계 영역을 보호하되,
                     std::unique_lock 또는 std::lock_guard로 감싸서 RAII로 사용.
        std::condition_variable: mutex와 짝으로 사용되어,
                                 다른 스레드가 공유 변수를 수정하고 해당 스레드에 알릴 때까지 다른 스레드 차단.
    */
    std::atomic<bool> gcRunning_{false};
    std::thread gcThread_;
    std::mutex gcMu_;
    std::condition_variable gcCv_;
};

}  // namespace sila2
