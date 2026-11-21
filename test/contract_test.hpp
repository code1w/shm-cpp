#ifndef SHM_CONTRACT_TEST_HPP_
#define SHM_CONTRACT_TEST_HPP_
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

template <typename Fn>
void ExpectContractAbort(Fn&& fn)
{
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0)
    {
        const rlimit limit{0, 0};
        (void)::setrlimit(RLIMIT_CORE, &limit);
        fn();
        std::_Exit(0);
    }
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    assert(result == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
