// Offline Quran memorization: ESP32-S3 N16R8, SPI ILI9341, XPT2046 touch.
// Set SCREEN_ST7789 to 1 only if your seller confirms an ST7789 panel.
#define SCREEN_ST7789 0
#ifndef BACKLIGHT_PWM_ENABLED
#define BACKLIGHT_PWM_ENABLED 1 // Enable only after a suitable LED driver is wired.
#endif
#define BACKLIGHT_PWM_INVERTED 0
constexpr int BACKLIGHT_PIN=4;
constexpr int BATTERY_PIN=1; // 10k + 10k divider midpoint, battery BEFORE boost.
constexpr float BATTERY_DIVIDER_RATIO=2.0f;
constexpr float BATTERY_CALIBRATION=1.0f; // Actual multimeter voltage / reported voltage.
constexpr unsigned long BATTERY_INTERVAL_MS=5000;
#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#if SCREEN_ST7789
#include <Adafruit_ST7789.h>
#else
#include <Adafruit_ILI9341.h>
#endif
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include "quran_data.h"

constexpr int SCK_PIN=12, MISO_PIN=13, MOSI_PIN=11;
constexpr int TFT_CS=10, TFT_DC=9, TFT_RST=8, TOUCH_CS=7;
constexpr int W=320, H=240, MENU_ROWS=5;
int ROWS=3;
uint16_t BG=0x08C4, PANEL=0x1947, WHITE=0xFFFE, GOLD=0xDD88, DIM=0x8C71;
enum Label { L_SURAH,L_RESUME,L_PREV,L_NEXT,L_HIDE,L_SHOW,L_BACK,L_HIDDEN,
 L_SHOW_VERSE,L_MORE,L_QURAN,L_REVIEW,L_START,L_WORD,L_REVEAL,L_RESET,L_MENU,L_DONE,L_PART,L_SETTINGS,L_BRIGHT,L_TIMEOUT,L_NEVER,L_THEME,L_DARK,L_LIGHT,
 L_FONT,L_SMALL,L_MEDIUM,L_LARGE,L_HIDDEN_START,L_YES,L_NO,L_BOOT,L_CALIBRATE,
 L_DEFAULTS,L_ERASE,L_ABOUT,L_CONFIRM,L_CANCEL,L_ERASE_QUESTION,L_DEFAULT_QUESTION,
 L_DEVICE_NAME,L_AUTHOR,L_VERSION,L_LED_REQUIRED,L_SECONDS,L_DISABLED };
struct Token { uint16_t word, ayah; bool marker; };
struct LegacyBookmark { uint32_t magic; uint16_t last; uint16_t pages[114]; };
struct Bookmark { uint32_t magic; uint16_t last; uint16_t pages[114],anchors[114]; uint8_t anchored[114]; };
struct Settings { uint32_t magic; uint8_t brightness,fontSize,dark,hiddenStart,resumeBoot; uint16_t timeout; };
struct Calibration { uint32_t magic; float ax,bx,cx,ay,by,cy; };
void label(int id,int x,int baseline,int width,uint16_t color=WHITE);
void button(int x,int y,int w,int h,int id,uint16_t color=WHITE);

#if SCREEN_ST7789
Adafruit_ST7789 tft(TFT_CS,TFT_DC,TFT_RST);
#else
Adafruit_ILI9341 tft(TFT_CS,TFT_DC,TFT_RST);
#endif
XPT2046_Touchscreen touch(TOUCH_CS);
GFXcanvas16 canvas(W,H);
Preferences prefs;
Bookmark saved{};
Settings settings{0x53455431,80,1,1,0,0,60};
bool inSettings=false,screenSleeping=false,settingsDirty=false;
int settingsPage=0,dialog=0,dragSlider=0;
unsigned long lastActivity=0,settingsChangedAt=0;
void drawSettings();
void applyBrightness();
void applyTheme();
void calibrate();
void handleSettingsTap(int x,int y);
void setScreenSleep(bool asleep);
int scalePx(int x) { return int(floorf(x*(settings.fontSize==0?0.75f:settings.fontSize==2?1.25f:1.0f))); }
void scaledWord(uint16_t id,int x,int baseline,uint16_t color);
Calibration cal{};
Token *tokens=nullptr;
uint16_t *lineStart=nullptr;
int tokenCount=0, lineCount=0, pages=0, surah=0, page=0, menuPage=0;
int revealedUntil=0;
bool reading=false, practice=false, held=false, bookmark=false;
unsigned long lastTap=0;
constexpr uint32_t BOOK_MAGIC=0x51545232, CAL_MAGIC=0x43414C32;

float batteryVolts=0;
int batteryPercent=-1, batteryX=0, batteryY=0;
unsigned long batterySampleAt=0;
// Approximate single-cell Li-ion curve; not a coulomb counter.
int batteryPercentFromVoltage(float v) {
 const float volts[]={3.30f,3.50f,3.60f,3.70f,3.75f,3.80f,3.85f,3.90f,4.00f,4.10f,4.20f};
 const int percent[]={0,5,10,20,30,40,50,60,75,90,100};
 if(v<=volts[0]) return 0;
 for(int i=1;i<11;++i) if(v<=volts[i])
  return int(percent[i-1]+(v-volts[i-1])/(volts[i]-volts[i-1])*(percent[i]-percent[i-1])+0.5f);
 return 100;
}
void sampleBattery() {
 analogReadMilliVolts(BATTERY_PIN); // Discard first conversion.
 uint32_t total=0;
 for(int i=0;i<16;++i) total+=analogReadMilliVolts(BATTERY_PIN);
 float v=(total/16.0f)*0.001f*BATTERY_DIVIDER_RATIO*BATTERY_CALIBRATION;
 if(v<2.5f || v>4.5f) { batteryVolts=0; batteryPercent=-1; }
 else {
  batteryVolts=batteryVolts==0?v:batteryVolts*0.8f+v*0.2f;
  batteryPercent=batteryPercentFromVoltage(batteryVolts);
 }
 batterySampleAt=millis();
 Serial.printf("Battery: %.3f V, %d%% (estimated)\n",batteryVolts,batteryPercent);
}
void drawBattery(int x,int y) {
 batteryX=x; batteryY=y;
 canvas.fillRect(x,y,58,9,BG);
 uint16_t color=batteryPercent>=0 && batteryPercent<=15?0xF800:DIM;
 canvas.drawRect(x,y,15,8,color); canvas.fillRect(x+15,y+2,2,4,color);
 if(batteryPercent>0) canvas.fillRect(x+2,y+2,max(1,batteryPercent*11/100),4,color);
 canvas.setTextSize(1);canvas.setTextColor(color);canvas.setCursor(x+21,y);
 if(batteryPercent<0) canvas.print("--%"); else {canvas.print(batteryPercent);canvas.print('%');}
}
void flush() { tft.drawRGBBitmap(0,0,canvas.getBuffer(),W,H); }
void latin(int x,int y,const String &s,uint16_t color=WHITE,int size=1) {
 canvas.setTextSize(size); canvas.setTextColor(color); canvas.setCursor(x,y); canvas.print(s);
}
void word(uint16_t id,int x,int baseline,uint16_t color) {
 const QWord &w=qWords[id];
 for(uint32_t i=w.first;i<w.first+w.count;++i) {
  const QRun &r=qRuns[i]; const QGlyph &g=qGlyphs[r.glyph];
  if(g.w && g.h) canvas.drawBitmap(x+w.bearing+r.x+g.x,baseline+r.y+g.y,
    qBitmap+g.offset,g.w,g.h,color);
 }
}
void sequence(const uint16_t *ids,int count,int right,int baseline,uint16_t color) {
 for(int i=0;i<count;++i) { right-=qWords[ids[i]].width; word(ids[i],right,baseline,color); right-=6; }
}
void label(int id,int x,int baseline,int width,uint16_t color) {
 const QLabel &l=qLabels[id]; const uint16_t *ids=qLabelWords+l.first;
 int len=0; for(int i=0;i<l.count;++i) len+=qWords[ids[i]].width+(i?6:0);
 sequence(ids,l.count,x+(width+len)/2,baseline,color);
}
void button(int x,int y,int w,int h,int id,uint16_t color) {
 canvas.fillRoundRect(x,y,w,h,5,PANEL);
 const QLabel &l=qLabels[id]; int top=0,bottom=0;
 for(int k=0;k<l.count;++k) {
  const QWord &qw=qWords[qLabelWords[l.first+k]];
  for(uint32_t i=qw.first;i<qw.first+qw.count;++i) {
   const QRun &r=qRuns[i]; const QGlyph &g=qGlyphs[r.glyph];
   top=min(top,int(r.y)+g.y); bottom=max(bottom,int(r.y)+g.y+g.h);
  }
 }
 label(id,x,y+(h-(bottom-top))/2-top,w,color);
}
int widthOf(int index) { return tokens[index].marker?28:(settings.fontSize==1?qWords[tokens[index].word].width:scalePx(qWords[tokens[index].word].width)+1); }
int pageStart() { return lineStart[page*ROWS]; }
int pageEnd() { return lineStart[min(lineCount,(page+1)*ROWS)]; }
void saveBookmark() {
 saved.magic=BOOK_MAGIC; saved.last=surah; saved.pages[surah]=page; saved.anchors[surah]=pageStart(); saved.anchored[surah]=1;
 if(prefs.putBytes("bookmark2",&saved,sizeof(saved))!=sizeof(saved)) Serial.println("Bookmark write failed");
 bookmark=true;
}
void buildLayout() {
 free(tokens); free(lineStart); tokens=nullptr; lineStart=nullptr;
 const QSurah &s=qSurahs[surah]; tokenCount=0;
 for(int a=0;a<s.count;++a) tokenCount+=qAyahs[s.first+a].count+1;
 tokens=(Token*)malloc(tokenCount*sizeof(Token));
 lineStart=(uint16_t*)malloc((tokenCount+1)*sizeof(uint16_t));
 if(!tokens || !lineStart) { Serial.println("Layout allocation failed"); while(true) delay(100); }
 int index=0;
 for(int a=0;a<s.count;++a) {
  const QAyah &v=qAyahs[s.first+a];
  for(int k=0;k<v.count;++k) tokens[index++]={qSequence[v.first+k],uint16_t(a+1),false};
  tokens[index++]={0,uint16_t(a+1),true};
 }
 ROWS=settings.fontSize==0?4:settings.fontSize==2?2:3;
 lineCount=1; lineStart[0]=0; int used=0;
 for(int i=0;i<tokenCount;++i) {
  int ww=widthOf(i);
  if(used && used+6+ww>292) { lineStart[lineCount++]=i; used=0; }
  used+=(used?6:0)+ww;
 }
 lineStart[lineCount]=tokenCount; pages=(lineCount+ROWS-1)/ROWS;
 page=constrain(page,0,pages-1);
}
void drawMenu() {
 inSettings=false; dialog=0; reading=false; canvas.fillScreen(BG);
 button(164,3,150,34,L_SETTINGS,GOLD);
 if(bookmark) button(7,3,145,32,L_RESUME,GOLD);
 for(int row=0;row<MENU_ROWS;++row) {
  int s=menuPage*MENU_ROWS+row; if(s>=Q_SURAH_COUNT) break;
  int y=42+31*row; canvas.fillRoundRect(7,y,306,28,4,PANEL);
  const QSurah &q=qSurahs[s]; sequence(qNameWords+q.nameFirst,q.nameCount,301,y+22,WHITE);
  latin(16,y+9,String(s+1),GOLD);
 }
 button(7,203,90,34,L_PREV); button(223,203,90,34,L_NEXT);
 drawBattery(130,205);
 latin(137,224,String(menuPage+1)+"/"+String((Q_SURAH_COUNT+MENU_ROWS-1)/MENU_ROWS),DIM);
 flush();
}
void drawPage() {
 inSettings=false; reading=true; canvas.fillScreen(BG);
 const QSurah &s=qSurahs[surah]; sequence(qNameWords+s.nameFirst,s.nameCount,309,28,GOLD);
 latin(8,9,String(page+1)+"/"+String(pages),DIM);
 latin(8,23,String(tokens[pageStart()].ayah)+"-"+String(tokens[pageEnd()-1].ayah),DIM);
 drawBattery(8,31);
 canvas.drawFastHLine(8,40,304,PANEL);
 for(int row=0;row<ROWS;++row) {
  int line=page*ROWS+row; if(line>=lineCount) break;
  int gap=settings.fontSize==0?27:settings.fontSize==2?49:36;
  int right=306, baseline=(settings.fontSize==0?67:settings.fontSize==2?86:75)+row*gap;
  for(int i=lineStart[line];i<lineStart[line+1];++i) {
   int ww=widthOf(i); right-=ww;
   bool visible=!practice || i<revealedUntil;
   if(visible) {
    if(tokens[i].marker) {
     canvas.drawCircle(right+14,baseline-8,13,GOLD);
     String n=String(tokens[i].ayah); latin(right+14-n.length()*3,baseline-11,n,GOLD);
    } else scaledWord(tokens[i].word,right,baseline,WHITE);
   } else if(!tokens[i].marker) {
    canvas.fillRoundRect(right,baseline-11,ww,5,2,PANEL);
   }
   right-=6;
  }
 }
 button(4,168,80,34,practice?L_RESET:L_START,GOLD);
 button(88,168,128,34,L_WORD,practice?WHITE:DIM);
 button(220,168,96,34,L_REVEAL,practice?WHITE:DIM);
 button(4,204,100,34,L_PREV); button(110,204,100,34,L_NEXT);
 button(216,204,100,34,L_MENU);
 flush();
}
void openSurah(int s) {
 surah=s;
 // Old bookmarks used fixed 24px pages. Convert them lazily without losing position.
 if(!saved.anchored[s]) {
  uint8_t size=settings.fontSize; settings.fontSize=1; page=saved.pages[s]; buildLayout();
  saved.anchors[s]=pageStart(); saved.anchored[s]=1; settings.fontSize=size;
 }
 page=0; buildLayout(); int anchor=constrain(int(saved.anchors[s]),0,tokenCount-1);
 while(page+1<pages && lineStart[(page+1)*ROWS]<=anchor) ++page;
 practice=settings.hiddenStart; revealedUntil=pageStart(); saveBookmark(); drawPage();
}
void navigate(int delta) {
 int target=constrain(page+delta,0,pages-1); if(target==page) return;
 int oldReveal=revealedUntil; page=target;
 if(practice) revealedUntil=(delta>0)?max(pageStart(),oldReveal):pageStart();
 saveBookmark(); drawPage();
}
void startPractice() { practice=true; revealedUntil=pageStart(); drawPage(); }
void revealWord() {
 if(!practice || revealedUntil>=pageEnd()) return;
 while(revealedUntil<tokenCount && tokens[revealedUntil].marker) ++revealedUntil;
 if(revealedUntil<pageEnd()) ++revealedUntil;
 while(revealedUntil<tokenCount && tokens[revealedUntil].marker) ++revealedUntil;
 drawPage();
}
void revealAyah() {
 if(!practice || revealedUntil>=pageEnd()) return;
 while(revealedUntil<tokenCount && tokens[revealedUntil].marker) ++revealedUntil;
 if(revealedUntil>=pageEnd()) { drawPage(); return; }
 int a=tokens[revealedUntil].ayah;
 // Reveal the whole ayah, including its continuation on subsequent pages.
 while(revealedUntil<tokenCount && tokens[revealedUntil].ayah==a) ++revealedUntil;
 drawPage();
}
void handleTap(int x,int y) {
 if(inSettings) { handleSettingsTap(x,y); return; }
 if(!reading) {
  if(y<37 && x>=164) { inSettings=true; settingsPage=0; drawSettings(); return; }
  if(y<37 && x<160 && bookmark) { openSurah(saved.last); return; }
  if(y>=42 && y<197) {
   int row=(y-42)/31;
   if((y-42)%31<28 && menuPage*MENU_ROWS+row<Q_SURAH_COUNT) openSurah(menuPage*MENU_ROWS+row);
  } else if(y>=203) {
   if(x<104 && menuPage>0) --menuPage;
   else if(x>216 && (menuPage+1)*MENU_ROWS<Q_SURAH_COUNT) ++menuPage;
   drawMenu();
  }
 } else {
  if(y>=168 && y<202) {
   if(x<85) startPractice(); else if(x<217) revealWord(); else revealAyah();
  } else if(y>=204) {
   if(x<105) navigate(-1); else if(x<211) navigate(1);
   else { menuPage=surah/MENU_ROWS; drawMenu(); }
  }
 }
}
void waitRelease() { while(touch.touched()) { touch.getPoint(); delay(15); } delay(180); }
TS_Point calibrationSample(int x,int y,int number) {
 waitRelease(); canvas.fillScreen(BG);
 latin(30,90,"Touch calibration "+String(number)+"/4",WHITE,2);
 latin(30,120,"Tap and hold the cross",DIM);
 canvas.drawFastHLine(x-10,y,21,GOLD); canvas.drawFastVLine(x,y-10,21,GOLD); flush();
 int count=0; long sx=0,sy=0;
 while(count<20) {
  if(touch.touched()) {
   TS_Point p=touch.getPoint();
   if(p.z>250) { sx+=p.x; sy+=p.y; ++count; }
  } else { count=0; sx=sy=0; }
  delay(10);
 }
 TS_Point result(sx/count,sy/count,0); waitRelease(); return result;
}
void calibrate() {
 // Three non-collinear points fit affine coordinates; fourth validates the fit.
 const int px[4]={20,299,20,299}, py[4]={20,20,219,219};
 while(true) {
  TS_Point r[4]; for(int i=0;i<4;++i) r[i]=calibrationSample(px[i],py[i],i+1);
  float u1=r[1].x-r[0].x,v1=r[1].y-r[0].y;
  float u2=r[2].x-r[0].x,v2=r[2].y-r[0].y;
  float determinant=u1*v2-u2*v1;
  if(fabsf(determinant)<100000) { Serial.println("Calibration degenerate; retry"); continue; }
  cal.ax=279*v2/determinant; cal.bx=-279*u2/determinant;
  cal.ay=-199*v1/determinant; cal.by=199*u1/determinant;
  cal.cx=20-cal.ax*r[0].x-cal.bx*r[0].y;
  cal.cy=20-cal.ay*r[0].x-cal.by*r[0].y;
  float checkX=cal.ax*r[3].x+cal.bx*r[3].y+cal.cx;
  float checkY=cal.ay*r[3].x+cal.by*r[3].y+cal.cy;
  if(fabsf(checkX-299)>22 || fabsf(checkY-219)>22) {
   Serial.println("Calibration check failed; repeat all four crosses"); continue;
  }
  cal.magic=CAL_MAGIC; prefs.putBytes("cal",&cal,sizeof(cal)); break;
 }
 held=false;
}
bool getTouch(int &x,int &y) {
 if(!touch.touched()) { held=false; dragSlider=0; return false; }
 TS_Point p=touch.getPoint();
 if(p.z<250 || (held && (!dragSlider || screenSleeping || !inSettings || dialog)) || millis()-lastTap<(held?70:160)) return false;
 x=lroundf(cal.ax*p.x+cal.bx*p.y+cal.cx);
 y=lroundf(cal.ay*p.x+cal.by*p.y+cal.cy);
 if(held && dragSlider) y=dragSlider==1?76:153;
 held=true; lastTap=millis();
 if(x<0 || x>=W || y<0 || y>=H) return false;
 return true;
}
void serialControl() {
 if(!Serial.available()) return;
 char c=Serial.read();
 if(c=='\n' || c=='\r') return;
 lastActivity=millis(); if(screenSleeping) setScreenSleep(false);
 if(c=='k') { calibrate(); lastActivity=millis(); if(inSettings) drawSettings(); else if(reading) drawPage(); else drawMenu(); }
 if(c=='b') drawMenu();
 if(c=='c' && bookmark) openSurah(saved.last);
 if(reading && !inSettings) {
  if(c=='n') navigate(1); if(c=='p') navigate(-1);
  if(c=='h') startPractice(); if(c=='w') revealWord(); if(c=='a') revealAyah();
 }
}

void scaledWord(uint16_t id,int x,int baseline,uint16_t color) {
 if(settings.fontSize==1) { word(id,x,baseline,color); return; }
 const QWord &w=qWords[id];
 for(uint32_t i=w.first;i<w.first+w.count;++i) {
  const QRun &r=qRuns[i]; const QGlyph &g=qGlyphs[r.glyph]; int stride=(g.w+7)/8;
  for(int yy=0;yy<g.h;++yy) for(int xx=0;xx<g.w;++xx) {
   if(!(qBitmap[g.offset+yy*stride+xx/8]&(128>>(xx%8)))) continue;
   int sx=w.bearing+r.x+g.x+xx, sy=r.y+g.y+yy;
   int a=scalePx(sx),b=scalePx(sy);
   canvas.fillRect(x+a,baseline+b,max(1,scalePx(sx+1)-a),max(1,scalePx(sy+1)-b),color);
  }
 }
}
void applyTheme() {
 if(settings.dark) {BG=0x08C4;PANEL=0x1947;WHITE=0xFFFE;GOLD=0xDD88;DIM=0x8C71;}
 else {BG=0xFFDE;PANEL=0xE73B;WHITE=0x1082;GOLD=0x6AE0;DIM=0x6B4D;}
}
void applyBrightness() {
#if BACKLIGHT_PWM_ENABLED
 int duty=screenSleeping?0:(settings.brightness*255+50)/100;
 analogWrite(BACKLIGHT_PIN,BACKLIGHT_PWM_INVERTED?255-duty:duty);
#endif
}
void changeSettings() { settingsDirty=true; settingsChangedAt=millis(); lastActivity=millis(); }
void saveSettings() {
 settings.magic=0x53455431;
 if(prefs.putBytes("settings",&settings,sizeof(settings))==sizeof(settings)) settingsDirty=false;
}
void loadSettings() {
 Settings candidate{};
 if(prefs.getBytesLength("settings")==sizeof(candidate)) prefs.getBytes("settings",&candidate,sizeof(candidate));
 if(candidate.magic==0x53455431 && candidate.brightness>=10 && candidate.brightness<=100 && candidate.fontSize<=2 &&
 candidate.dark<=1 && candidate.hiddenStart<=1 && candidate.resumeBoot<=1 &&
 (candidate.timeout==0 || (candidate.timeout>=30 && candidate.timeout<=180 && candidate.timeout%30==0))) settings=candidate;
}
void slider(int y,int value,int steps,uint16_t color) {
 canvas.fillRoundRect(24,y-3,272,6,3,PANEL);
 int x=24+value*272/steps; canvas.fillCircle(x,y,8,color);
}
void settingsRow(int y,int id) { canvas.fillRoundRect(8,y,304,36,5,PANEL);label(id,112,y+25,194,WHITE); }
void drawSettings() {
 inSettings=true; canvas.fillScreen(BG);
 if(dialog) {
  drawBattery(10,13);
  label(dialog==1?L_DEFAULT_QUESTION:L_ERASE_QUESTION,8,88,304,GOLD);
  button(8,150,144,40,L_CANCEL); button(168,150,144,40,L_CONFIRM,GOLD); flush(); return;
 }
 label(L_SETTINGS,90,28,220,GOLD);
 latin(10,13,String(settingsPage+1)+"/3",DIM);
 drawBattery(10,28);
 if(settingsPage==0) {
  label(L_BRIGHT,150,58,160,WHITE); latin(14,47,String(settings.brightness)+"%",GOLD);
  slider(76,(settings.brightness-10)/5,18,BACKLIGHT_PWM_ENABLED?GOLD:DIM);
#if !BACKLIGHT_PWM_ENABLED
  label(L_LED_REQUIRED,10,108,300,DIM);
#endif
  label(L_TIMEOUT,130,135,180,WHITE);
  if(settings.timeout) latin(14,124,String(settings.timeout)+" s",GOLD);
  else label(L_DISABLED,0,135,125,GOLD);
  slider(153,settings.timeout?settings.timeout/30-1:0,5,GOLD);
  button(8,168,304,34,L_NEVER,settings.timeout==0?GOLD:DIM);
 } else if(settingsPage==1) {
  settingsRow(42,L_THEME); label(settings.dark?L_DARK:L_LIGHT,12,67,90,GOLD);
  settingsRow(81,L_FONT); label(settings.fontSize==0?L_SMALL:settings.fontSize==1?L_MEDIUM:L_LARGE,12,106,90,GOLD);
  settingsRow(120,L_HIDDEN_START); label(settings.hiddenStart?L_YES:L_NO,12,145,70,GOLD);
  settingsRow(159,L_BOOT); label(settings.resumeBoot?L_RESUME:L_MENU,12,184,90,GOLD);
 } else {
  button(8,42,304,36,L_CALIBRATE); button(8,81,304,36,L_DEFAULTS);
  button(8,120,304,36,L_ERASE); button(8,159,304,36,L_ABOUT);
 }
 button(4,204,100,34,L_PREV); button(110,204,100,34,L_NEXT);button(216,204,100,34,L_BACK);
 flush();
}
void drawAbout() {
 dialog=3;canvas.fillScreen(BG);drawBattery(10,13);label(L_DEVICE_NAME,8,75,304,GOLD);
 label(L_AUTHOR,8,118,304,WHITE);label(L_VERSION,8,156,304,DIM);
 button(8,201,304,36,L_BACK);flush();
}
void handleSettingsTap(int x,int y) {
 if(dialog==3) { if(y>=201) {dialog=0;drawSettings();} return; }
 if(dialog) {
  if(y>=150 && y<190) {
   if(x<160) {dialog=0;drawSettings();return;}
   if(dialog==1) {
    settings=Settings{0x53455431,80,1,1,0,0,60};applyTheme();applyBrightness();saveSettings();
   } else {
    memset(&saved,0,sizeof(saved)); bookmark=false;
    prefs.remove("bookmark2");prefs.remove("bookmark");
   }
   dialog=0;drawSettings();
  } return;
 }
 if(y>=204) {
  saveSettings();
  if(x<105) settingsPage=max(0,settingsPage-1);
  else if(x<211) settingsPage=min(2,settingsPage+1);
  else {drawMenu();return;}
  drawSettings();return;
 }
 if(settingsPage==0) {
#if BACKLIGHT_PWM_ENABLED
  if(y>=63 && y<=89) {dragSlider=1;settings.brightness=10+constrain((x-24)*18/272,0,18)*5;applyBrightness();changeSettings();drawSettings();}
#endif
  if(y>=140 && y<=164) {dragSlider=2;settings.timeout=30*(1+constrain(((x-24)*5+136)/272,0,5));changeSettings();drawSettings();}
  else if(y>=168 && y<202) {settings.timeout=settings.timeout?0:60;changeSettings();drawSettings();}
 } else if(settingsPage==1 && y>=42 && y<195) {
  int row=(y-42)/39;if((y-42)%39>=36)return;
  if(row==0) {settings.dark=!settings.dark;applyTheme();}
  if(row==1) settings.fontSize=(settings.fontSize+1)%3;
  if(row==2) settings.hiddenStart=!settings.hiddenStart;
  if(row==3) settings.resumeBoot=!settings.resumeBoot;
  changeSettings();drawSettings();
 } else if(settingsPage==2 && y>=42 && y<195) {
  int row=(y-42)/39;if((y-42)%39>=36)return;
  if(row==0) {calibrate();lastActivity=millis();drawSettings();}
  if(row==1) {dialog=1;drawSettings();}
  if(row==2) {dialog=2;drawSettings();}
  if(row==3) drawAbout();
 }
}
void setScreenSleep(bool asleep) {
 screenSleeping=asleep;applyBrightness();
 tft.startWrite();tft.writeCommand(asleep?0x28:0x11);tft.endWrite();
 if(asleep) {tft.startWrite();tft.writeCommand(0x10);tft.endWrite();delay(120);}
 else {
  delay(120);tft.startWrite();tft.writeCommand(0x29);tft.endWrite();
  if(inSettings) {if(dialog==3)drawAbout();else drawSettings();}
  else if(reading)drawPage();else drawMenu();
 }
}

void setup() {
 Serial.begin(115200);
 pinMode(BATTERY_PIN,INPUT);
 analogReadResolution(12);
 analogSetPinAttenuation(BATTERY_PIN,ADC_11db);
 sampleBattery();
 if(!canvas.getBuffer()) { Serial.println("Framebuffer allocation failed"); while(true) delay(100); }
 pinMode(TFT_CS,OUTPUT); digitalWrite(TFT_CS,HIGH);
 pinMode(TOUCH_CS,OUTPUT); digitalWrite(TOUCH_CS,HIGH);
 SPI.begin(SCK_PIN,MISO_PIN,MOSI_PIN);
 #if SCREEN_ST7789
 tft.init(240,320);
#else
 tft.begin(20000000);
#endif
 tft.setRotation(1);
 touch.begin(); touch.setRotation(0);
 prefs.begin("quran-trainer",false);
 loadSettings();
 if(prefs.getBytesLength("bookmark2")==sizeof(saved)) prefs.getBytes("bookmark2",&saved,sizeof(saved));
 else if(prefs.getBytesLength("bookmark")==sizeof(LegacyBookmark)) {
  LegacyBookmark old{}; prefs.getBytes("bookmark",&old,sizeof(old));
  if(old.magic==BOOK_MAGIC && old.last<Q_SURAH_COUNT) {
   saved.magic=old.magic; saved.last=old.last; memcpy(saved.pages,old.pages,sizeof(old.pages));
  }
 }
 if(saved.magic==BOOK_MAGIC && saved.last<Q_SURAH_COUNT) bookmark=true;
 else memset(&saved,0,sizeof(saved));
 applyTheme(); applyBrightness();
 if(prefs.getBytesLength("cal")==sizeof(cal)) prefs.getBytes("cal",&cal,sizeof(cal));
 if(cal.magic!=CAL_MAGIC || !isfinite(cal.ax) || !isfinite(cal.by)) calibrate();
 Serial.println("Commands: k calibrate, b menu, c resume, n next, p previous, h start, w word, a ayah");
 lastActivity=millis();
 if(settings.resumeBoot && bookmark) openSurah(saved.last); else drawMenu();
}
void loop() {
 int x,y;
 if(getTouch(x,y)) {
  lastActivity=millis();
  if(screenSleeping) setScreenSleep(false); // Wake consumes this entire press.
  else handleTap(x,y);
 }
 serialControl();
 if(millis()-batterySampleAt>=BATTERY_INTERVAL_MS) {
  sampleBattery();
  if(!screenSleeping && !held) { drawBattery(batteryX,batteryY); flush(); }
 }
 if(settingsDirty && millis()-settingsChangedAt>=700) saveSettings();
 if(!screenSleeping && dialog!=1 && dialog!=2 && settings.timeout && millis()-lastActivity>=uint32_t(settings.timeout)*1000)
  setScreenSleep(true);
 delay(10);
}
