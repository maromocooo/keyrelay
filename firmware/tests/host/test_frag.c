/* Host-side test for yd_frag (fragmentation / reassembly over the BLE link). Run: make -C firmware/tests/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yd_link.h"
#include "yd_frag.h"
static uint8_t frags[400][256]; static uint16_t flen[400]; static int nf; static uint16_t maxp=222;
/* cur_conn = いまの接続。full_after 片を送ったところで送信キューが一杯になり（TIMEOUT）、そのあいだに reconnect_at 回目の
 * 呼び出しで別の接続に変わる、を模擬する */
static uint32_t cur_conn = 5; static int full_after = -1, calls, reconnect_at = -1;
esp_err_t yd_link_send_on(uint32_t conn, uint8_t ch, const uint8_t *d, uint16_t l){
  calls++;
  if (calls == reconnect_at) cur_conn++;
  if (conn != cur_conn) return ESP_ERR_INVALID_STATE;
  if (full_after >= 0 && nf >= full_after && calls < reconnect_at) return ESP_ERR_TIMEOUT;
  if(l>maxp) {printf("TOO BIG %u\n",l); exit(1);} memcpy(frags[nf],d,l); flen[nf++]=l; return ESP_OK; }
uint16_t yd_link_max_payload(void){return maxp;}
int main(){
  static uint8_t msg[3136], buf[3136]; yd_frag_rx_t r; yd_frag_rx_init(&r, buf, sizeof buf);
  yd_frag_init();
  int ok=0;
  for (int it=0; it<3000; it++){
    maxp = (it%3==0)?222:(it%3==1?60:30);
    size_t len = 1 + rand()%3136; for(size_t i=0;i<len;i++) msg[i]=rand();
    nf=0; if (yd_frag_send(cur_conn,3,msg,len)!=ESP_OK){printf("send fail len=%zu maxp=%u\n",len,maxp); continue;}
    int drop = (it%7==0 && nf>2) ? 1+rand()%(nf-1) : -1;
    int got=0;
    for(int i=0;i<nf;i++){ if(i==drop) continue; int n=yd_frag_rx_feed(&r,frags[i],flen[i]); if(n>0){ got=n; } }
    if (drop<0){ if(got!=(int)len||memcmp(buf,msg,len)){printf("MISMATCH it=%d\n",it);return 1;} ok++; }
    else if (got>0){ printf("delivered despite drop it=%d\n",it); return 1; }
  }
  /* START arriving mid-message resets */
  nf=0; maxp=222; yd_frag_send(cur_conn,3,msg,1000); int half=nf/2; for(int i=0;i<half;i++) yd_frag_rx_feed(&r,frags[i],flen[i]);
  int nf1=nf; yd_frag_send(cur_conn,3,msg,500); int got=0; for(int i=nf1;i<nf;i++){int n=yd_frag_rx_feed(&r,frags[i],flen[i]); if(n>0)got=n;}
  /* 送信キューが一杯で再試行しているあいだに再接続した: 残りの片は新しい接続に送らない */
  nf=0; calls=0; full_after=2; reconnect_at=10; uint32_t old=cur_conn;
  esp_err_t e = yd_frag_send(old,3,msg,1000);
  int switched_ok = e==ESP_ERR_INVALID_STATE && nf==2 && cur_conn==old+1;
  full_after=-1; reconnect_at=-1;
  /* 送り始める前に切れていたら、1片も送らない */
  nf=0; e = yd_frag_send(old,3,msg,100);
  int stale_ok = e==ESP_ERR_INVALID_STATE && nf==0;
  printf("ok=%d restart_got=%d dropped=%u switched=%d stale=%d\n", ok, got, r.dropped, switched_ok, stale_ok);
  return got==500 && switched_ok && stale_ok?0:1;
}
