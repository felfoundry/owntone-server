
#ifndef __HTTPD_STREAMING_H__
#define __HTTPD_STREAMING_H__

#include "misc.h" // struct media_quality

enum streaming_format
{
  STREAMING_FORMAT_MP3,
  STREAMING_FORMAT_MP3_ICY,
};

struct streaming_session;

struct httpd_streaming_client
{
  struct streaming_session *session;
  enum streaming_format format;
  struct media_quality quality;

  struct httpd_streaming_client *next;
};

struct httpd_streaming_client *
httpd_streaming_clientinfo_get(void);

void
httpd_streaming_clientinfo_free(struct httpd_streaming_client *clients);

int
httpd_streaming_fd_set(struct httpd_streaming_client *client, int fd);


#endif /* !__HTTPD_STREAMING_H__ */
