#!/bin/bash
#
# kernel-dev-loop.sh - Kernel Development Iteration Loop
# 
# 本脚本完全复用 test-kernel skill，专注于：
# 1. 编译验证
# 2. 调用 test-kernel 进行测试（复用其镜像准备、QEMU启动、SSH检测等）
# 3. Panic 分析和修复建议
# 4. 迭代循环管理
#
# Usage: ./scripts/kernel-dev-loop.sh [options]
#   -c, --continuous    连续模式：失败后等待修复并自动重试
#   -h, --help          显示帮助

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 配置
KERNEL_IMAGE="arch/x86/boot/bzImage"
QEMU_DIR="/home/sisyphus/code/qemu"
ITERATION=0
MAX_ITERATIONS=${MAX_ITERATIONS:-10}
CONTINUOUS_MODE=${CONTINUOUS_MODE:-0}

# 日志函数
log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 阶段 1: 编译验证
phase_compile() {
    log_info "=== Phase 1: Compile Verification ==="
    
    local build_log="build-$(date +%Y%m%d-%H%M%S).log"
    
    log_info "Compiling kernel with $(nproc) jobs..."
    if ! make -j$(nproc) 2>&1 | tee "$build_log"; then
        log_error "Compilation failed!"
        log_info "Build log: $build_log"
        
        if grep -q "error:" "$build_log"; then
            log_error "Compilation errors:"
            grep "error:" "$build_log" | head -5
        fi
        return 1
    fi
    
    if [[ ! -f "$KERNEL_IMAGE" ]]; then
        log_error "Kernel image not found: $KERNEL_IMAGE"
        return 1
    fi
    
    log_success "Kernel compiled: $KERNEL_IMAGE ($(ls -lh $KERNEL_IMAGE | awk '{print $5}'))"
    return 0
}

# 阶段 2: 测试内核（完全复用 test-kernel skill）
phase_test() {
    log_info "=== Phase 2: Kernel Testing (via test-kernel skill) ==="
    
    if ! command -v test-kernel &> /dev/null; then
        log_error "test-kernel skill not found"
        return 1
    fi
    
    log_info "Delegating to test-kernel skill for:"
    log_info "  - Environment cleanup"
    log_info "  - Kernel image preparation"
    log_info "  - QEMU startup and monitoring"
    log_info "  - SSH connectivity check"
    log_info "  - Test execution"
    
    # 完全委托给 test-kernel，不做任何重复操作
    if test-kernel "$KERNEL_IMAGE" 2>&1; then
        log_success "test-kernel completed successfully"
        return 0
    else
        return 1
    fi
}

# 阶段 3: Panic 分析（基于 test-kernel 生成的日志）
phase_analyze() {
    log_info "=== Phase 3: Panic Analysis ==="
    
    local serial_log="$QEMU_DIR/serial.log"
    local panic_log="$QEMU_DIR/qemu.log"
    local test_result="/home/sisyphus/code/test/test_result.txt"
    
    if [[ ! -f "$serial_log" ]]; then
        log_warning "No serial.log found"
        return 1
    fi
    
    # 检查成功标志
    if grep -q "Test completed successfully" "$serial_log" 2>/dev/null ||
       grep -q "Test completed successfully" "$test_result" 2>/dev/null; then
        log_success "✓ Kernel boot and tests successful"
        
        if grep -q "NUMA-aware tasklist initialized" "$serial_log"; then
            log_success "✓ $(grep "NUMA-aware tasklist initialized" "$serial_log" | tail -1)"
        fi
        return 0
    fi
    
    # 检查 panic
    if [[ -f "$panic_log" ]] && grep -q "Kernel panic" "$panic_log"; then
        log_error "✗ Kernel panic detected!"
        analyze_panic_type "$panic_log" "$serial_log"
        return 1
    fi
    
    return 1
}

# 分析 panic 类型并提供修复建议
analyze_panic_type() {
    local panic_log="$1"
    local serial_log="$2"
    
    echo ""
    log_info "Analyzing panic..."
    
    if grep -q "No working init found" "$panic_log"; then
        echo ""
        log_warning "[PANIC TYPE A] 'No working init found'"
        echo ""
        echo "Root cause: tasks链表被禁用 → for_each_process遍历失败"
        echo ""
        echo "Fix: Restore tasks list operations in fork.c and exit.c"
        echo "  list_add_tail_rcu(&p->tasks, &init_task.tasks);     // Keep this!"
        echo "  numa_tasklist_add(p, current_numa_node());           // Add this too"
        echo ""
        return
    fi
    
    if grep -q "Unable to handle kernel" "$panic_log"; then
        echo ""
        log_warning "[PANIC TYPE B] Kernel Oops"
        grep -A 10 "Call Trace:" "$panic_log" | head -15
        return
    fi
    
    # 显示完整日志
    echo ""
    cat "$panic_log"
}

# 主循环
main() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -c|--continuous) CONTINUOUS_MODE=1; shift ;;
            -h|--help)
                echo "Kernel Development Iteration Loop"
                echo ""
                echo "4-phase workflow: Compile → Test → Analyze → Report"
                echo "Test phase delegates to test-kernel skill (no duplication)"
                echo ""
                echo "Usage: kernel-dev-loop [options]"
                echo "  -c    Continuous mode (retry on failure)"
                echo "  -h    Show help"
                exit 0
                ;;
            *) log_error "Unknown option: $1"; exit 1 ;;
        esac
    done
    
    log_info "Starting Kernel Development Loop"
    log_info "Max iterations: $MAX_ITERATIONS"
    
    while [[ $ITERATION -lt $MAX_ITERATIONS ]]; do
        ITERATION=$((ITERATION + 1))
        echo ""
        log_info "=== Iteration $ITERATION/$MAX_ITERATIONS ==="
        
        # Phase 1: Compile
        phase_compile || {
            [[ $CONTINUOUS_MODE -eq 1 ]] && { sleep 10; continue; } || exit 1
        }
        
        # Phase 2: Test (via test-kernel)
        if phase_test && phase_analyze; then
            echo ""
            log_success "✅ ALL PHASES COMPLETED!"
            exit 0
        fi
        
        # Failed
        phase_analyze
        
        if [[ $CONTINUOUS_MODE -eq 1 ]]; then
            log_info "Waiting 10s before retry (Ctrl+C to stop)..."
            sleep 10
        else
            log_error "Failed. Fix issues and run again, or use: kernel-dev-loop -c"
            exit 1
        fi
    done
    
    log_error "Max iterations reached"
    exit 1
}

main "$@"
