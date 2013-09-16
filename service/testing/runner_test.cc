#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include <sys/types.h>
#include <sys/wait.h>

#include <mutex>
#include <thread>

#include <boost/test/unit_test.hpp>

#include "jml/arch/atomic_ops.h"
#include "jml/arch/exception.h"
#include "jml/arch/futex.h"
#include "jml/arch/timers.h"
#include "jml/utils/exc_assert.h"
#include "jml/utils/string_functions.h"
#include "soa/service/message_loop.h"
#include "soa/service/runner.h"
#include "soa/service/sink.h"

#include <iostream>

#include "signals.h"

using namespace std;
using namespace Datacratic;

// #define BOOST_CHECK_EQUAL(x,y)  { ExcCheckEqual((x), (y), ""); }

struct HelperCommands : vector<string>
{
    HelperCommands()
        : vector<string>(),
          active_(0)
    {}

    void reset() { active_ = 0; }

    string nextCommand()
    {
        if (active_ < size()) {
            int active = active_;
            active_++;
            return at(active);
        }
        else {
            return "";
        }
    }

    void sendOutput(bool isStdOut, const string & data)
    {
        char cmdBuffer[1024];
        int len = data.size();
        int totalLen = len + 3 + sizeof(int);
        sprintf(cmdBuffer, (isStdOut ? "out" : "err"));
        memcpy(cmdBuffer + 3, &len, sizeof(int));
        memcpy(cmdBuffer + 3 + sizeof(int), data.c_str(), len);
        push_back(string(cmdBuffer, totalLen));
    }

    void sendExit(int code)
    {
        char cmdBuffer[1024];
        int totalLen = 3 + sizeof(int);
        sprintf(cmdBuffer, "xit");
        memcpy(cmdBuffer + 3, &code, sizeof(int));
        push_back(string(cmdBuffer, totalLen));
    };

    void sendAbort()
    {
        push_back("abt");
    }

    int active_;
};

#if 1
/* ensures that the basic callback system works */
BOOST_AUTO_TEST_CASE( test_runner_callbacks )
{
    BlockedSignals blockedSigs(SIGCHLD);

    MessageLoop loop;

    HelperCommands commands;
    commands.sendOutput(true, "hello stdout");
    commands.sendOutput(true, "hello stdout2");
    commands.sendOutput(false, "hello stderr");
    commands.sendExit(0);

    string receivedStdOut, expectedStdOut;
    string receivedStdErr, expectedStdErr;

    expectedStdOut = ("helper: ready\nhello stdout\nhello stdout2\n"
                      "helper: exit with code 0\n");
    expectedStdErr = "hello stderr\n";

    int done = false;
    auto onTerminate = [&] (const Runner::RunResult & result) {
        done = true;
        ML::futex_wake(done);
    };

    auto onStdOut = [&] (string && message) {
        // cerr << "received message on stdout: /" + message + "/" << endl;
        receivedStdOut += message;
    };
    auto stdOutSink = make_shared<CallbackInputSink>(onStdOut);

    auto onStdErr = [&] (string && message) {
        // cerr << "received message on stderr: /" + message + "/" << endl;
        receivedStdErr += message;
    };
    auto stdErrSink = make_shared<CallbackInputSink>(onStdErr);

    Runner runner;
    loop.addSource("runner", runner);
    loop.start();

    auto & stdInSink = runner.getStdInSink();
    runner.run({"build/x86_64/bin/runner_test_helper"},
               onTerminate, stdOutSink, stdErrSink);
    for (const string & command: commands) {
        while (!stdInSink.write(string(command))) {
            ML::sleep(0.1);
        }
    }
    stdInSink.requestClose();

    while (!done) {
        ML::futex_wait(done, false);
    }

    BOOST_CHECK_EQUAL(ML::hexify_string(receivedStdOut),
                      ML::hexify_string(expectedStdOut));
    BOOST_CHECK_EQUAL(ML::hexify_string(receivedStdErr),
                      ML::hexify_string(expectedStdErr));

    loop.shutdown();
}
#endif

#if 1
/* ensures that the returned status is properly set after termination */
BOOST_AUTO_TEST_CASE( test_runner_normal_exit )
{
    BlockedSignals blockedSigs(SIGCHLD);

    auto nullSink = make_shared<NullInputSink>();

    /* normal termination, with code */
    {
        MessageLoop loop;

        HelperCommands commands;
        commands.sendExit(123);

        Runner::RunResult result;
        auto onTerminate = [&] (const Runner::RunResult & newResult) {
            result = newResult;
        };
        Runner runner;
        loop.addSource("runner", runner);
        loop.start();

        auto & stdInSink = runner.getStdInSink();
        runner.run({"build/x86_64/bin/runner_test_helper"},
                   onTerminate, nullSink, nullSink);
        for (const string & command: commands) {
            stdInSink.write(string(command));
        }
        stdInSink.requestClose();
        runner.waitTermination();

        BOOST_CHECK_EQUAL(result.signaled, false);
        BOOST_CHECK_EQUAL(result.returnCode, 123);

        loop.shutdown();
    }

    /* aborted termination, with signum */
    {
        MessageLoop loop;

        HelperCommands commands;
        commands.sendAbort();

        Runner::RunResult result;
        auto onTerminate = [&] (const Runner::RunResult & newResult) {
            result = newResult;
        };
        Runner runner;
        loop.addSource("runner", runner);
        loop.start();

        auto & stdInSink = runner.getStdInSink();
        runner.run({"build/x86_64/bin/runner_test_helper"},
                   onTerminate, nullSink, nullSink);
        for (const string & command: commands) {
            stdInSink.write(string(command));
        }
        stdInSink.requestClose();
        runner.waitTermination();

        BOOST_CHECK_EQUAL(result.signaled, true);
        BOOST_CHECK_EQUAL(result.returnCode, SIGABRT);

        loop.shutdown();
    }
}
#endif

#if 1
/* test the "execute" function */
BOOST_AUTO_TEST_CASE( test_runner_execute )
{
    string received;
    auto onStdOut = [&] (string && message) {
        received = move(message);
    };
    auto stdOutSink = make_shared<CallbackInputSink>(onStdOut, nullptr);

    auto result = execute({"/bin/cat", "-"},
                          stdOutSink, nullptr, "hello callbacks");
    BOOST_CHECK_EQUAL(received, "hello callbacks");
    BOOST_CHECK_EQUAL(result.signaled, false);
    BOOST_CHECK_EQUAL(result.returnCode, 0);
}
#endif

#if 1
/* perform multiple runs with the same Runner and ensures task-specific
 * components are properly segregated */
BOOST_AUTO_TEST_CASE( test_runner_cleanup )
{
    MessageLoop loop;

    Runner runner;
    loop.addSource("runner", runner);
    loop.start();

    auto nullSink = make_shared<NullInputSink>();

    auto performLoop = [&] (const string & loopData) {
        HelperCommands commands;
        commands.sendOutput(true, loopData);
        commands.sendExit(0);

        string expectedStdOut("helper: ready\n" + loopData
                              + "\nhelper: exit with code 0\n");
        string receivedStdOut;
        auto onStdOut = [&] (string && message) {
            // cerr << "received message on stdout: /" + message + "/" << endl;
            receivedStdOut += message;
        };
        auto stdOutSink = make_shared<CallbackInputSink>(onStdOut);

        auto & stdInSink = runner.getStdInSink();
        runner.run({"build/x86_64/bin/runner_test_helper"},
                   nullptr, stdOutSink, nullSink);
        for (const string & command: commands) {
            stdInSink.write(string(command));
        }
        stdInSink.requestClose();
        runner.waitTermination();

        BOOST_CHECK_EQUAL(ML::hexify_string(receivedStdOut),
                          ML::hexify_string(expectedStdOut));
    };

    for (int i = 0; i < 5; i++) {
        performLoop(to_string(i));
    }

    loop.shutdown();
}
#endif

#if 1
/* stress test that runs 10 threads in parallel, where each thread:
- invoke "execute", with 3000 messages to stderr and stdout (each)
  received from the stdin sink
- compare those messages with a fixture
and
- the parent thread that outputs messages on stderr and on stdout until all
  threads are done
- wait for the termination of all threads
- ensures that all child process have properly exited
*/

BOOST_AUTO_TEST_CASE( test_stress_runner )
{
    vector<thread> threads;
    vector<int> childPids;
    int nThreads(20), activeThreads;
    int msgsToSend(3000);

    activeThreads = nThreads;
    auto runThread = [&] (int threadNum) {
        /* preparation */
        HelperCommands commands;
        string receivedStdOut, expectedStdOut;
        string receivedStdErr, expectedStdErr;
        size_t stdInBytes(0);

        receivedStdOut.reserve(msgsToSend * 80);
        expectedStdOut.reserve(msgsToSend * 80);
        receivedStdErr.reserve(msgsToSend * 80);
        expectedStdErr.reserve(msgsToSend * 80);

        expectedStdOut = "helper: ready\n";
        for (int i = 0; i < msgsToSend; i++) {
            string stdOutData = (to_string(threadNum)
                                 + ":" + to_string(i)
                                 + ": this is a message to stdout\n\t"
                                 + "and a tabbed line");
            commands.sendOutput(true, stdOutData);
            expectedStdOut += stdOutData + "\n";
            string stdErrData = (to_string(threadNum)
                                 + ":" + to_string(i)
                                 + ": this is a message to stderr\n\t"
                                 + "and a tabbed line");
            commands.sendOutput(false, stdErrData);
            expectedStdErr += stdErrData + "\n";
        }
        commands.sendExit(0);

        expectedStdOut += "helper: exit with code 0\n";

        /* execution */
        MessageLoop loop;
        Runner runner;

        loop.addSource("runner", runner);
        loop.start();

        auto onStdOut = [&] (string && message) {
            receivedStdOut += message;
        };
        auto stdOutSink = make_shared<CallbackInputSink>(onStdOut);
        auto onStdErr = [&] (string && message) {
            receivedStdErr += message;
        };
        auto stdErrSink = make_shared<CallbackInputSink>(onStdErr);

        auto & stdInSink = runner.getStdInSink();
        runner.run({"build/x86_64/bin/runner_test_helper"},
                   nullptr, stdOutSink, stdErrSink);

        for (const string & command: commands) {
            while (!stdInSink.write(string(command))) {
                ML::sleep(0.1);
            }
            stdInBytes += command.size();
        }
        stdInSink.requestClose();

        ML::sleep(1.0);

        runner.waitTermination();
        childPids.push_back(runner.childPid());

        loop.shutdown();

        AsyncFdOutputSink * sinkPtr = (AsyncFdOutputSink *) &stdInSink;
        BOOST_CHECK_EQUAL(sinkPtr->bytesSent(), stdInBytes);

        BOOST_CHECK_EQUAL(receivedStdOut, expectedStdOut);
        BOOST_CHECK_EQUAL(receivedStdErr, expectedStdErr);

        ML::atomic_dec(activeThreads);
        cerr << "activeThreads now: " + to_string(activeThreads) + "\n";
        if (activeThreads == 0) {
            ML::futex_wake(activeThreads);
        }
    };

    for (int i = 0; i < nThreads; i++) {
        threads.emplace_back(runThread, i);
    }

    ML::memory_barrier();

    /* attempting to interfere with stdout/stderr as long as any thread is
     * running */
    while (activeThreads > 0) {
        cout << "performing interference on stdout\n";
        cerr << "performing interference on stderr\n";
        // int n = activeThreads;
        // ML::futex_wait(activeThreads, n);
    }

    for (thread & current: threads) {
        current.join();
    }
 
    /* ensure children have all exited... */
    BOOST_CHECK_EQUAL(childPids.size(), threads.size());
    for (const int & pid: childPids) {
        if (pid > 0) {
            waitpid(pid, NULL, WNOHANG);
            int errno_ = errno;
            BOOST_CHECK_EQUAL(errno_, ECHILD);
        }
        else {
            throw ML::Exception("no pid");
        }
    }
}

#endif
