/*
   thread.cpp

   Definition of a Java style thread class in C++.

   ------------------------------------------

   Copyright © 2013  [Vic Hargrave - http://vichargrave.com]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "thread.h"

static void* runThread(void* arg)
{
    return ((Thread*)arg)->run();
}

Thread::Thread() : m_tid(0), m_running(0), m_detached(0) {}

Thread::~Thread()
{
    if (m_running == 1) {
        if (m_detached == 0) {
             pthread_detach(m_tid);
        }
        pthread_cancel(m_tid);
    }
}

int Thread::start(unsigned int ss, bool jemalloc_tcache)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize (&attr, ss*1024);
    int result = pthread_create(&m_tid, &attr, runThread, this);
    if (result == 0) {
        m_running = 1;
    }
		if (jemalloc_tcache==false) {
#ifndef NOJEM
			bool cache=false;
			mallctl("thread.tcache.enabled", NULL, NULL, &cache, sizeof(bool));
#endif /* NOJEM */
		}
    return result;
}

int Thread::join()
{
    int result = -1;
    if (m_running == 1) {
        result = pthread_join(m_tid, NULL);
        if (result == 0) {
            m_running = 0;
        }
    }
    return result;
}

/* commenting the following code because we don't use it
int Thread::detach()
{
    int result = -1;
    if (m_running == 1 && m_detached == 0) {
        result = pthread_detach(m_tid);
        if (result == 0) {
            m_detached = 1;
        }
    }
    return result;
}

pthread_t Thread::self() {
    return m_tid;
}
*/
