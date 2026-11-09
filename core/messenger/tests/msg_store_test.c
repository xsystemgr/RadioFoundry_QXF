#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "messenger/msg_store.h"
#include "messenger/msg_packet.h"
int main(void){
  msg_store_reset();
  assert(msg_store_count(MSG_BOX_INBOX)==0);
  for(int i=0;i<8;i++){char t[8];sprintf(t,"m%d",i);msg_store_add_inbox("SV4QXF",t,MSG_PKT_TYPE_TEXT,-70);}
  assert(msg_store_count(MSG_BOX_INBOX)==MSGR_INBOX_MAX); // capped
  const msg_entry_t*e=msg_store_get(MSG_BOX_INBOX,0);
  assert(e&&strcmp(e->text,"m7")==0); // newest first
  assert(msg_store_unread()==MSGR_INBOX_MAX);
  msg_store_mark_read(MSG_BOX_INBOX,0);
  assert(msg_store_unread()==MSGR_INBOX_MAX-1);
  msg_store_delete(MSG_BOX_INBOX,0);
  assert(msg_store_count(MSG_BOX_INBOX)==MSGR_INBOX_MAX-1);
  e=msg_store_get(MSG_BOX_INBOX,0); assert(e&&strcmp(e->text,"m6")==0);
  msg_store_add_sent("hello",42); assert(msg_store_count(MSG_BOX_SENT)==1);
  // ACK marks the matching sent id delivered
  const msg_entry_t*se=msg_store_get(MSG_BOX_SENT,0); assert(se&&!se->acked&&se->id==42);
  assert(msg_store_ack(42)); se=msg_store_get(MSG_BOX_SENT,0); assert(se&&se->acked);
  assert(!msg_store_ack(999)); // unknown id ignored
  msg_store_add_draft("wip"); assert(msg_store_count(MSG_BOX_DRAFTS)==1);
  // heard upsert: same call stays one row, moves to top
  msg_store_heard("SV4QXF",-80,MSG_PKT_TYPE_PING);
  msg_store_heard("SV1ABC",-60,MSG_PKT_TYPE_TEXT);
  msg_store_heard("SV4QXF",-55,MSG_PKT_TYPE_PONG);
  assert(msg_store_heard_count()==2);
  const msg_heard_t*h=msg_store_heard_get(0); assert(h&&strcmp(h->call,"SV4QXF")==0&&h->rssi==-55);
  // text longer than cap is truncated
  msg_store_add_inbox("LONGCALLSIGN","this text is definitely longer than thirty-two characters!!",MSG_PKT_TYPE_TEXT,-90);
  e=msg_store_get(MSG_BOX_INBOX,0); assert(e&&strlen(e->text)==MSGR_TEXT_LEN&&strlen(e->from)==MSGR_CALL_LEN);
  printf("msg_store test: OK (cap, newest-first, unread, delete, sent/drafts, ack, heard upsert, truncation)\n");
  return 0;
}
