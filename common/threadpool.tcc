#ifndef RADDI_THREADPOOL_TCC
#define RADDI_THREADPOOL_TCC

template <typename Thread>
threadpool <Thread> ::threadpool ()
    : handle (CreateEvent (NULL, FALSE, FALSE, NULL))
    , semaphore (0) {}

template <typename Thread>
threadpool <Thread> ::~threadpool () {
    CloseHandle (this->handle);
}

template <typename Thread>
void threadpool <Thread> ::begin (std::size_t n) {
    this->semaphore = n;
}

template <typename Thread>
bool threadpool <Thread> ::dispatch (void (Thread::*fn)(), Thread * t) {
    return QueueUserWorkItem (ThreadProc, new parameter { this, t, fn }, 0);
}

template <typename Thread>
void threadpool <Thread> ::join () {
    WaitForSingleObject (this->handle, INFINITE);
}

template <typename Thread>
DWORD WINAPI threadpool <Thread> ::ThreadProc (LPVOID lpParameter) {
    DWORD r = 0;
    auto p = static_cast <parameter *> (lpParameter);
    try {
        (p->thread->*(p->function)) ();
    } catch (...) {
        r = ERROR_GEN_FAILURE;
    }
    if (InterlockedDecrement (&p->pool->semaphore) == 0) {
        SetEvent (p->pool->handle);
    }
    delete p;
    return r;
}

#endif

