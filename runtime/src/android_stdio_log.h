#pragma once
#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
static int br2_pfd[2];
static void* br2_pump(void*) {
  char b[1024]; ssize_t n;
  while ((n = read(br2_pfd[0], b, sizeof(b)-1)) > 0) {
    b[n] = 0; __android_log_write(ANDROID_LOG_INFO, "BR2Stdio", b);
  }
  return nullptr;
}
static inline void br2_redirect_stdio() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  if (pipe(br2_pfd) != 0) return;
  dup2(br2_pfd[1], 1); dup2(br2_pfd[1], 2);
  pthread_t t; pthread_create(&t, nullptr, br2_pump, nullptr);
  pthread_detach(t);
}
#else
static inline void br2_redirect_stdio() {}
#endif
