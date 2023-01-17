
#ifndef __HTTPD_STREAMING_H__
#define __HTTPD_STREAMING_H__

#include "misc.h" // struct media_quality

enum streaming_formats
{
  STREAMING_FORMAT_MP3     = (1 << 0),
  STREAMING_FORMAT_MP3_ICY = (1 << 1),
};

struct httpd_streaming_clients
{
  void *session;
  enum streaming_formats format;
  struct media_quality quality;

  struct httpd_streaming_clients *next;
};

struct httpd_streaming_clients *
httpd_streaming_clientinfo_get(void);

void
httpd_streaming_clientinfo_free(struct httpd_streaming_clients *clients);

int
httpd_streaming_fd_set(struct httpd_streaming_clients *client, int fd);


#endif /* !__HTTPD_STREAMING_H__ */
