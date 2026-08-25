#include "ObservableCommandManager.h"

#include "ObservableCommandExecution.h"

#include <sila/error/SiLAErrorSubtypes.h>
#include <sila/util/uuid.h>

#include <utility>

namespace sila2 {
/*  = default:
    기본 생성자(인자 없는 생성자)를 컴파일러가 자동 생성하게 허용.
    컴파일러에게 "사용자 정의 아님(trivial일 수 있음)"이라는 힌트를 추가로 줘서 최적화 여지가 더 넓음.
*/
ObservableCommandManager::ObservableCommandManager() = default;

// Defined here, not defaulted in the header: at the point the header is
// parsed, ObservableCommandExecution is only forward-declared, so
// unique_ptr's default deleter (which needs sizeof(T)) cannot be
// instantiated yet. By the time this .cc includes ObservableCommandExecution.h,
// the type is complete and commands_'s implicit destruction is well-formed.
ObservableCommandManager::~ObservableCommandManager() {
    stopAutoGC();
}

ObservableCommandExecution& ObservableCommandManager::addCommand(std::chrono::seconds lifetime) {
    std::string uuid = util::generateUuid();
    // Construct the execution before taking mu_: the constructor does no
    // shared-state work, so there is nothing to protect yet, and keeping it
    // out of the critical section means the lock is only ever held for the
    // map mutation itself.
    /*  std::make_unique: C++14부터.
        new 연산자 직접 사용보다 안전하게 unique_ptr 생성 가능.
        1. new를 직접 안 쓰니까 delete 누락 가능성 자체가 없음.
        2. T를 생성자 타입, 인자 타입으로 두번 적어야 돼서, DRY 원칙 위반.
        3. 예외 안전성 — 객체 생성과 스마트 포인터 포장이 하나의 원자적(Atomic)인 단계로 묶임.
                     (C++17부터 평가 순서가 보장돼서 문제 해소됐지만, make_unique가 여전히 표준 관용구.)
            같은 코드 내 객체 생성 뒤 포인터를 std::unique_ptr 생성자에 포인터를 할당하기 전
            다른 곳에서 예외 발생 시, 포인터는 생성된 채 통제를 잃게 됨 -> 메모리 누수 발생.
        } 
    */
    auto execution = std::make_unique<ObservableCommandExecution>(uuid, lifetime);
    /*  std::lock_guard<std::mutex> lock{mu_}
        mu_를 잠그고, lock이 스코프를 벗어나면(함수 리턴 시) 자동으로 풀림(RAII).
        commands_.emplace() 반환 타입은 std::pair<iterator, bool>,
        여기서 iterator는 unordered_map (commands_)의 key-value 한 쌍 (entry).
        auto가 오른쪽 표현식 반환 타입을 통해 컴파일 타임에 치환.

        iterator의 각 인자 이름은 first, second. 따라서 entry의 value = execution.
    */
    std::lock_guard<std::mutex> lock{mu_};
    auto [it, inserted] = commands_.emplace(std::move(uuid), std::move(execution));
    return *it->second;
}

ObservableCommandExecution& ObservableCommandManager::getCommand(const std::string& uuid) {
    std::lock_guard<std::mutex> lock{mu_};
    auto it = commands_.find(uuid);
    if (it == commands_.end()) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    }
    return *it->second;
}

std::size_t ObservableCommandManager::removeExpired() {
    std::lock_guard<std::mutex> lock{mu_};
    std::size_t removed = 0;
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->second->isExpired()) {
            it = commands_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void ObservableCommandManager::interruptAll() {
    std::lock_guard<std::mutex> lock{mu_};
    for (auto& [uuid, execution] : commands_) { execution->requestInterruption(); }
}

void ObservableCommandManager::startAutoGC(std::chrono::seconds interval) {
    if (gcRunning_.exchange(true)) { return; }
    /*  lambda expression:
        [captures](params) { ... }

        captures: lambda를 만들 때 바깥 스코프에서 가져오는 값. 한 번 정해지면 고정.
        [] :  Capture none.
        [&x]: Capture only x by reference 
        [x] : Capture only x by value
        [&] : Capture all vars by reference
        [=] : Capture all vars by value
        [x,y] : Capture x and y by value
        [&x,y] : Capture x by reference , Capture y by value
        [&x, &y] : Capture x and y by reference
        [&, y] : Capture all except for y by reference
        [=, &x] : Capture all except for x by value

        params: 람다를 호출할 때 넘기는 값. 호출마다 달라짐.
    */
    gcThread_ = std::thread{[this, interval] {
        /*  변수 타입:
            lock      — std::unique_lock<std::mutex>   (gcMu_의 RAII 래퍼)
            gcMu_     — std::mutex                     (이 스레드 전용 뮤텍스)
            gcRunning_— std::atomic<bool>              (GC 실행 여부 플래그)
            gcCv_     — std::condition_variable        (sleep/wake 신호 채널)
            interval  — std::chrono::seconds           (람다가 값으로 캡처한 주기)

            std::unique_lock:
            lock_guard와 같은 RAII 뮤텍스 래퍼이나, 중간에 잠금 해제/재획득이 가능.
            condition_variable이 wait 안에서 lock을 풀었다 다시 잡아야 하므로 lock_guard 대신 사용.

            루프 흐름:
            1. gcRunning_(std::atomic<bool>)이 원자적으로 값을 가져와서 반환 → false면 스레드 종료.
            2. gcCV_(std::condition_variable::wait_for(
                lock:     락을 걸고 풀 쓰레드 인스턴스.
                rel_time: 최대 대기 기간.
                pred:     조건식 함수. True 시 락 해제.)):
                - interval 경과 또는 gcCv_.notify_all() 수신 시 깨어남.
                - 락 해제해도 lock을 다시 잡고, pred(= !gcRunning_)를 확인.
                - wait_for가 true|false 반환 → unlock|lock.
            3. gcRunning_이 여전히 true면 removeExpired() 실행.
        */
        std::unique_lock<std::mutex> lock{gcMu_};
        while (gcRunning_.load()) {
            gcCv_.wait_for(lock, interval, [this] { return !gcRunning_.load(); });
            if (gcRunning_.load()) { removeExpired(); }
        }
    }};
}

void ObservableCommandManager::stopAutoGC() {
    if (!gcRunning_.exchange(false)) { return; }
    /*  std::condition_variable::notify_all():
        락 걸린 모든 쓰레드를 해제
    */
    gcCv_.notify_all();
    /*  std::thread.joinable(): 작업 중인 쓰레드인지 확인.
        std::thread.join():     스레드가 정리를 마치고 반환할 때까지 block. 
    */
    if (gcThread_.joinable()) { gcThread_.join(); }
}

bool ObservableCommandManager::isAutoGCRunning() const {
    return gcRunning_.load();
}

std::size_t ObservableCommandManager::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return commands_.size();
}

}  // namespace sila2
