#include <fcntl.h>
#include <unistd.h>

#include <dasynq.h>

using event_loop_t = dasynq::event_loop_n;
event_loop_t event_loop;

// directory containing built executables
std::string dinit_bindir;

// directory for all test output (each test under a named subdir)
std::string igr_output_basedir;


// exception to be thrown on failure
class igr_failure_exc
{
    std::string message;
public:
    igr_failure_exc(std::string message_p) : message(message_p) { }
};

// A process watcher that cleans up by terminating the child process
class igr_proc_watch : public event_loop_t::child_proc_watcher_impl<igr_proc_watch>
{
public:
    bool did_exit = true;
    pid_t child_pid = -1;
    int status = 0;

    dasynq::rearm status_change(event_loop_t &, pid_t child, int status_p)
    {
        status = status_p;
        did_exit = true;
        child_pid = -1;
        return dasynq::rearm::REMOVE;
    }

    pid_t fork(event_loop_t &eloop, bool from_reserved = false, int prio = dasynq::DEFAULT_PRIORITY)
    {
        if (child_pid != -1) {
            throw std::logic_error("igr_proc_watch: attempted to fork when already watching a process");
        }

        did_exit = false;
        child_pid = event_loop_t::child_proc_watcher_impl<igr_proc_watch>::fork(eloop, from_reserved, prio);
        return child_pid;
    }

    ~igr_proc_watch()
    {
        if (child_pid != -1) {
            deregister(event_loop, child_pid);
            kill(child_pid, SIGKILL);
        }
    }
};

// A simple timer that allows watching for an arbitrary timeout
class simple_timer : public event_loop_t::timer_impl<simple_timer> {
    bool is_registered = false;;
    bool is_expired = false;
public:
    simple_timer()
    {
        add_timer(event_loop, dasynq::clock_type::MONOTONIC);
        is_registered = true;
    }

    dasynq::rearm timer_expiry(event_loop_t &loop, int expiry_count)
    {
        is_expired = true;
        is_registered = false;
        return dasynq::rearm::REMOVE;
    }

    void arm(const timespec &timeout)
    {
        is_expired = false;
        arm_timer_rel(event_loop, timeout);
    }

    bool did_expire()
    {
        return is_expired;
    }

    ~simple_timer()
    {
        if (is_registered) {
            deregister(event_loop);
        }
    }
};

// Consume and buffer and output from a pipe
class pipe_consume_buffer : public event_loop_t::fd_watcher_impl<pipe_consume_buffer> {
    int fds[2];
    std::string buffer;

public:
    pipe_consume_buffer()
    {
        if(pipe(fds) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe_consume_buffer: pipe");
        }
        if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe_consume_buffer: fcntl");
        }
        if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe_consume_buffer: fcntl");
        }

        try {
            add_watch(event_loop, fds[0], dasynq::IN_EVENTS);
        }
        catch (...) {
            close(fds[0]);
            close(fds[1]);
            throw;
        }
    }

    ~pipe_consume_buffer()
    {
        deregister(event_loop);
    }

    int get_output_fd()
    {
        return fds[1];
    }

    std::string get_output()
    {
        return buffer;
    }

    dasynq::rearm fd_event(event_loop_t &loop, int fd, int flags)
    {
        // read all we can
        char buf[1024];

        ssize_t r = read(fd, buf, 1024);
        while (r != 0) {
            if (r == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // actual error, not expected
                    throw std::system_error(errno, std::generic_category(), "pipe_consume_buffer: read");
                }
                // otherwise: would block, finish reading now
                return dasynq::rearm::REARM;
            }

            buffer.append(buf, r);
            r = read(fd, buf, 1024);
        }

        // leave disarmed:
        return dasynq::rearm::NOOP;
    }
};

// dinit process
class dinit_proc
{
    igr_proc_watch pwatch;
    pipe_consume_buffer out;
    pipe_consume_buffer err;

public:
    dinit_proc() {}

    ~dinit_proc() {}

    // start, in specified working directory, with given arguments
    void start(const char *wdir, std::vector<std::string> args = {})
    {
        std::string dinit_exec = dinit_bindir + "/dinit";
        char **arg_arr = new char *[args.size() + 2];
        arg_arr[0] = const_cast<char *>(dinit_exec.c_str());

        unsigned i;
        for (i = 0; i < args.size(); ++i) {
            arg_arr[i + 1] = const_cast<char *>(args[i].c_str());
        }
        arg_arr[++i] = nullptr;

        pid_t pid = pwatch.fork(event_loop);
        if (pid == 0) {
            chdir(wdir);
            dup2(out.get_output_fd(), STDOUT_FILENO);
            dup2(err.get_output_fd(), STDERR_FILENO);

            execv(dinit_exec.c_str(), arg_arr);
            exit(EXIT_FAILURE);
        }

        delete[] arg_arr;
    }

    void wait_for_term(dasynq::time_val timeout)
    {
        if (pwatch.did_exit) return;

        simple_timer timer;
        timer.arm(timeout);

        while (!pwatch.did_exit && !timer.did_expire()) {
            event_loop.run();
        }

        if (!pwatch.did_exit) {
            throw igr_failure_exc("timeout waiting for termination");
        }
    }

    std::string get_stdout()
    {
        return out.get_output();
    }

    std::string get_stderr()
    {
        return err.get_output();
    }
};

// perform basic setup for a test (with automatic teardown)
class igr_test_setup
{
    std::string output_dir;

public:
    igr_test_setup(const std::string &test_name)
    {
        output_dir = igr_output_basedir + "/" + test_name;
        if (mkdir(output_dir.c_str(), 0700) == -1 && errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), std::string("mkdir: ") + output_dir);
        }

        setenv("IGR_OUTPUT", output_dir.c_str(), true);
    }

    ~igr_test_setup()
    {
        unsetenv("IGR_OUTPUT");
    }

    const std::string &get_output_dir()
    {
        return output_dir;
    }
};

// set an environment variable (with automatic restore of original value at teardown)
class igr_env_var_setup
{
    std::string orig_value;
    std::string var_name;
    bool had_value;

public:
    igr_env_var_setup(const char *var_name_p, const char *value)
    {
        var_name = var_name_p;
        const char *orig_value_cp = getenv(var_name_p);
        had_value = (orig_value_cp != nullptr);
        if (had_value) {
            orig_value = orig_value_cp;
        }
        int r;
        if (value != nullptr) {
            r = setenv(var_name_p, value, true);
        }
        else {
            r = unsetenv(var_name_p);
        }
        if (r == -1) {
            throw std::system_error(errno, std::generic_category(), "setenv/unsetenv");
        }
    }

    ~igr_env_var_setup()
    {
        if (had_value) {
            setenv(var_name.c_str(), orig_value.c_str(), true);
        }
        else {
            unsetenv(var_name.c_str());
        }
    }

    void set(const char *value)
    {
        int r;
        if (value != nullptr) {
            r = setenv(var_name.c_str(), value, true);
        }
        else {
            r = unsetenv(var_name.c_str());
        }
        if (r == -1) {
            throw std::system_error(errno, std::generic_category(), "setenv/unsetenv");
        }
    }
};

// read entire file contents as a string
inline std::string read_file_contents(const std::string &filename)
{
    std::string contents;

    int fd = open(filename.c_str(), O_RDONLY);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "read_file_contents: open");
    }

    char buf[1024];
    int r = read(fd, buf, 1024);
    while (r > 0) {
        contents.append(buf, r);
        r = read(fd, buf, 1024);
    }

    if (r == -1) {
        throw std::system_error(errno, std::generic_category(), "read_file_contents: read");
    }

    return contents;
}

inline void check_file_contents(const std::string &file_path, const std::string &expected_contents)
{
    std::string contents = read_file_contents(file_path);
    if (contents != expected_contents) {
        throw igr_failure_exc(std::string("File contents do not match expected for file ") + file_path);
    }
}

inline void igr_assert_eq(const std::string &expected, const std::string &actual)
{
    if (expected != actual) {
        throw igr_failure_exc(std::string("Test assertion failed:\n") + "Expected: " + expected + "\nActual: " + actual);
    }
}
