package local.d2rm;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.concurrent.atomic.AtomicBoolean;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends Activity {
    private static final AtomicBoolean started = new AtomicBoolean();
    private static volatile boolean guestRunning;
    private static volatile java.lang.ref.WeakReference<MainActivity> active = new java.lang.ref.WeakReference<>(null);
    private android.app.AlertDialog keyboardDialog;
    private android.app.AlertDialog saveFailureDialog;
    private long touchButtons, keyButtons;
    private void publishButtons() { nativeButtons(touchButtons | keyButtons, false); }
    private PadView padView;
    private LinearLayout preparePanel;
    private ScrollView prepareScroll;
    private android.widget.TextView prepareTitle, prepareText;
    private android.widget.ProgressBar prepareBar;
    private android.widget.Button prepareRetry, prepareImport, prepareContinue;
    private final Object surfaceGate = new Object();
    private volatile boolean surfaceReady, foreground, destroyed, gameEntered;
    private boolean preparing, prepareFailed;
    private volatile boolean allowLazy;
    private String preparePhase = "正在检查启动资源";
    private static final int IMPORT_SHADERS = 71;
    private static final long MAX_SHADER_ARCHIVE = 1024L * 1024 * 1024;

    private void setPreparing(boolean value) {
        preparing=value;
        if(foreground && (value || guestRunning))getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    private void prepareUi(String title,String message,boolean failed) {
        runOnUiThread(() -> {
            if(destroyed || isFinishing())return;
            prepareFailed=failed;
            prepareScroll.setVisibility(android.view.View.VISIBLE);
            prepareTitle.setText(title);prepareText.setText(message);
            prepareBar.setIndeterminate(true);prepareBar.setVisibility(failed?android.view.View.GONE:android.view.View.VISIBLE);
            prepareRetry.setVisibility(failed && !guestRunning?android.view.View.VISIBLE:android.view.View.GONE);
            prepareImport.setVisibility(failed && !guestRunning?android.view.View.VISIBLE:android.view.View.GONE);
            prepareContinue.setVisibility(android.view.View.GONE);
            setPreparing(!failed);
        });
    }
    private void onShaderPrepareProgress(int done,int total,String phase) {
        preparePhase=phase==null?"正在准备 Vulkan 着色器":phase;
        final String message=preparePhase;
        runOnUiThread(() -> {
            if(destroyed || isFinishing())return;
            prepareTitle.setText("正在准备 Vulkan 着色器");
            boolean known=total>0 && done>=0 && done<=total;
            prepareBar.setIndeterminate(!known);
            if(known){prepareBar.setMax(total);prepareBar.setProgress(done);}
            prepareText.setText("预编译常用管线，其他变体首次使用时编译。\n"+message+(known?"\n"+done+" / "+total:""));
        });
    }
    private boolean awaitSurface() throws InterruptedException {
        synchronized(surfaceGate) {
            while(!destroyed && (!surfaceReady || !foreground))surfaceGate.wait();
            return !destroyed;
        }
    }
    private void startWhenReady() {
        if(destroyed || prepareFailed || !foreground || !surfaceReady || !started.compareAndSet(false,true))return;
        prepareUi("准备启动","正在检查本机资源；首次 Vulkan 准备完成后会保存设备缓存。",false);
        new Thread(() -> {
            try {if(awaitSurface())run();}
            catch(Exception e){log("准备失败："+e.getMessage());}
            finally {
                if(!gameEntered) {
                    started.set(false);
                    runOnUiThread(() -> {
                        if(!destroyed)setPreparing(false);
                        MainActivity current=active.get();
                        if(destroyed && current!=null && current!=this)current.startWhenReady();
                    });
                }
            }
        },"dmhost").start();
    }
    private void chooseShaderArchive() {
        if(started.get())return;
        android.content.Intent intent=new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);intent.setType("*/*");
        try {startActivityForResult(intent,IMPORT_SHADERS);}
        catch(android.content.ActivityNotFoundException e){log("准备失败：系统没有文件选择器，请将档案放到指定 shaders 目录。");}
    }
    private void validateShaderArchive(File file) throws java.io.IOException {
        if(file.length()<16 || file.length()>MAX_SHADER_ARCHIVE)throw new java.io.IOException("着色器档案大小无效（允许 16 字节至 1 GiB）："+file);
        byte[] header=new byte[8];
        try(InputStream in=new java.io.FileInputStream(file)) {
            int offset=0,n;while(offset<header.length && (n=in.read(header,offset,header.length-offset))>0)offset+=n;
            if(offset!=8 || !java.util.Arrays.equals(header,"D2RVKSH4".getBytes(java.nio.charset.StandardCharsets.US_ASCII)))
                throw new java.io.IOException("需要兼容的 SH4 graphics.vkpack 档案；游戏 data 目录不能代替 Vulkan 着色器档案。");
        }
    }
    private void copyAtomic(InputStream in,File target,boolean shader) throws java.io.IOException {
        File parent=target.getParentFile();
        if(!parent.isDirectory() && !parent.mkdirs())throw new java.io.IOException("无法建立目录："+parent);
        File temporary=new File(parent,target.getName()+".importing");
        try {
            try(FileOutputStream out=new FileOutputStream(temporary)) {
                byte[] bytes=new byte[65536];int n;long count=0;
                while((n=in.read(bytes))!=-1) {
                    count+=n;if(shader && count>MAX_SHADER_ARCHIVE)throw new java.io.IOException("着色器档案超过 1 GiB 限制");
                    out.write(bytes,0,n);
                }
                out.getFD().sync();
            }
            if(shader)validateShaderArchive(temporary);
            if(!temporary.renameTo(target))throw new java.io.IOException("无法替换文件："+target);
        } finally {if(temporary.exists())temporary.delete();}
    }
    private boolean hasAsset(String name) throws java.io.IOException {
        for(String entry:getAssets().list(""))if(entry.equals(name))return true;
        return false;
    }
    private File prepareShaderFiles(File dir,File external,long version) throws java.io.IOException {
        File catalog=new File(dir,"graphics.vkpack"),supplied=new File(external,"shaders/graphics.vkpack");
        android.content.SharedPreferences prefs=getSharedPreferences("shader-assets",MODE_PRIVATE);
        if(supplied.isFile()) {
            validateShaderArchive(supplied);
            String fingerprint=supplied.getAbsolutePath()+":"+supplied.length()+":"+supplied.lastModified();
            if(!catalog.isFile() || !fingerprint.equals(prefs.getString("external-source","")) || catalog.length()!=supplied.length()) {
                prepareUi("准备启动","正在导入本机 Vulkan 着色器档案…",false);
                try(InputStream in=new java.io.FileInputStream(supplied)){copyAtomic(in,catalog,true);}
                prefs.edit().putString("external-source",fingerprint).remove("apk-version").apply();
            }
        } else if(hasAsset("graphics.vkpack")) {
            if(!catalog.isFile() || prefs.getLong("apk-version",-1)!=version) {
                try(InputStream in=getAssets().open("graphics.vkpack")){copyAtomic(in,catalog,true);}
                prefs.edit().putLong("apk-version",version).remove("external-source").apply();
            }
        } else if(!catalog.isFile()) {
            throw new java.io.IOException("缺少 Vulkan 着色器档案。请点“选择着色器档案”导入 graphics.vkpack，或放到：\n"+supplied.getAbsolutePath()+"\n原有 romfs/data 无需移动，但不能直接代替此档案。");
        }
        validateShaderArchive(catalog);
        File manifest=new File(dir,"pso_manifest.tsv");
        if(!manifest.isFile() || prefs.getLong("manifest-version",-1)!=version) {
            try(InputStream in=getAssets().open("pso_manifest.tsv")){copyAtomic(in,manifest,false);}
            prefs.edit().putLong("manifest-version",version).apply();
        }
        return manifest;
    }
    @Override protected void onActivityResult(int request,int result,android.content.Intent data) {
        super.onActivityResult(request,result,data);
        if(request!=IMPORT_SHADERS || result!=RESULT_OK || data==null || data.getData()==null)return;
        if(!started.compareAndSet(false,true))return;
        prepareUi("正在导入","正在复制并检查本机着色器档案…",false);
        final android.net.Uri uri=data.getData();
        new Thread(() -> {
            boolean imported=false;
            try {
                File external=getExternalFilesDir(null);if(external==null)throw new java.io.IOException("外部存储不可用");
                try(InputStream in=getContentResolver().openInputStream(uri)) {
                    if(in==null)throw new java.io.IOException("无法读取选择的文件");
                    copyAtomic(in,new File(external,"shaders/graphics.vkpack"),true);
                }
                getSharedPreferences("shader-assets",MODE_PRIVATE).edit().remove("external-source").apply();
                imported=true;
            } catch(Exception e){log("导入失败："+e.getMessage());}
            finally {
                started.set(false);final boolean ok=imported;
                runOnUiThread(() -> {
                    if(!destroyed){setPreparing(false);if(ok){prepareFailed=false;startWhenReady();}}
                    else {MainActivity current=active.get();if(current!=null && ok)current.startWhenReady();}
                });
            }
        },"shader-import").start();
    }
    // Geometry and gestures are owned by upstream dm_touch_controls.c.
    // This adapter translates Android pixels to UIKit-sized points and paints
    // DmTouchOverlay's shapes/assets without inventing a controller layout.
    private final class PadView extends android.view.View implements android.view.Choreographer.FrameCallback {
        final android.graphics.Paint paint = new android.graphics.Paint(3);
        final android.util.SparseIntArray slots = new android.util.SparseIntArray();
        final float[] state = new float[128];
        final float[] paintedState = new float[128];
        boolean hasPaintedState;
        final java.util.HashMap<String,android.graphics.Bitmap> art = new java.util.HashMap<>();
        float scale=1;
        boolean ticking;
        PadView() { super(MainActivity.this); setContentDescription("D2R 触控层"); }
        android.graphics.Bitmap asset(String name) {
            if(art.containsKey(name)) return art.get(name);
            android.graphics.Bitmap bitmap=null;
            try(java.io.InputStream in=getAssets().open("touch/"+name)) { bitmap=android.graphics.BitmapFactory.decodeStream(in); }
            catch(java.io.IOException ex) { android.util.Log.e("dmhost","Missing touch art: "+name,ex); }
            art.put(name,bitmap); return bitmap;
        }
        @Override protected void onMeasure(int widthSpec,int heightSpec) {
            int w=android.view.View.MeasureSpec.getSize(widthSpec),h=android.view.View.MeasureSpec.getSize(heightSpec);
            if(w*9>h*16)w=h*16/9;else h=w*9/16;
            setMeasuredDimension(w,h);
        }
        @Override protected void onSizeChanged(int w,int h,int ow,int oh) {
            reset(); if(w<=0 || h<=0)return; scale=h/440f; nativeTouchSize(w/scale,h/scale);
        }
        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); ticking=true; android.view.Choreographer.getInstance().postFrameCallback(this); }
        @Override protected void onDetachedFromWindow() { ticking=false; android.view.Choreographer.getInstance().removeFrameCallback(this); reset(); super.onDetachedFromWindow(); }
        @Override public void doFrame(long time) {
            if(!ticking)return;
            nativeTouchFrame(state);pumpSaveFailure();
            // Keep the input/pulse pump at display cadence. Android retains
            // the overlay's display list until its visible state changes.
            if(!hasPaintedState || !java.util.Arrays.equals(state,paintedState)) {
                System.arraycopy(state,0,paintedState,0,state.length);
                hasPaintedState=true;invalidate();
            }
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
        void bitmap(android.graphics.Canvas c,String name,android.graphics.Rect src,float x,float y,float w,float h,int alpha) {
            android.graphics.Bitmap image=asset(name); if(image==null)return;paint.setAlpha(alpha);
            c.drawBitmap(image,src,new android.graphics.RectF(x,y,x+w,y+h),paint);paint.setAlpha(255);
        }
        void caption(android.graphics.Canvas c,String text,float x,float y,float size) {
            paint.setStyle(android.graphics.Paint.Style.FILL);paint.setColor(0xffd9c9a6);paint.setTextSize(size);paint.setTypeface(android.graphics.Typeface.SERIF);paint.setTextAlign(android.graphics.Paint.Align.CENTER);
            c.drawText(text,x,y-(paint.ascent()+paint.descent())/2,paint);
        }
        void ring(android.graphics.Canvas c,float x,float y,float r,boolean held) {
            float lw=Math.max(1.5f,r*.075f);
            paint.setStyle(android.graphics.Paint.Style.FILL);paint.setColor(held?0xeb16110c:0x7a16110c);c.drawCircle(x,y,r,paint);
            paint.setStyle(android.graphics.Paint.Style.STROKE);paint.setStrokeWidth(lw);paint.setColor(held?0xff997847:0x8a997847);c.drawCircle(x,y,r-lw/2,paint);
            paint.setStrokeWidth(lw*.5f);paint.setColor(0x8a1f170d);c.drawCircle(x,y,r-lw*1.3f,paint);paint.setStyle(android.graphics.Paint.Style.FILL);
        }
        org.json.JSONObject hudArt;
        boolean hudArtLoaded;
        org.json.JSONObject hudArt() {
            if(!hudArtLoaded) {
                hudArtLoaded=true;
                try(InputStream in=getAssets().open("touch/hud_v4_ui/ui.json");java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream()) {
                    byte[] buf=new byte[8192];int n;while((n=in.read(buf))!=-1)out.write(buf,0,n);
                    hudArt=new org.json.JSONObject(out.toString("UTF-8"));
                } catch(Exception ex){android.util.Log.e("dmhost","HUD art metadata",ex);}
            }
            return hudArt;
        }
        float hx(double x,float w,float u){return (float)((x-1434)*u+w/2);}
        int hudColor(org.json.JSONArray rgb) throws org.json.JSONException {return Color.rgb(rgb.getInt(0),rgb.getInt(1),rgb.getInt(2));}
        void hudImage(android.graphics.Canvas c,String file,double x,double y,float w,float u,int alpha) {
            String name="hud_v4_ui/"+file;android.graphics.Bitmap im=asset(name);
            if(im!=null)bitmap(c,name,null,hx(x,w,u),(float)y*u,im.getWidth()*u,im.getHeight()*u,alpha);
        }
        // DmTouchOverlay v4text: use the same pre-rendered glyphs and advances.
        void hudText(android.graphics.Canvas c,String text,org.json.JSONObject glyphs,double x,double baseline,int align,float w,float u) throws org.json.JSONException {
            double adv=0;
            for(int i=0;i<text.length();i++){org.json.JSONObject g=glyphs.optJSONObject(text.substring(i,i+1));if(g!=null)adv+=g.getDouble("adv");}
            double pen=x-(align==1?adv:adv/2);
            for(int i=0;i<text.length();i++){
                org.json.JSONObject g=glyphs.optJSONObject(text.substring(i,i+1));if(g==null)continue;
                if(g.has("file"))hudImage(c,g.getString("file"),pen+g.getDouble("dx"),baseline+g.getDouble("dy"),w,u,255);
                pen+=g.getDouble("adv");
            }
        }
        void hudDigit(android.graphics.Canvas c,int n,double x,double y,float w,float u,int alpha) {
            String file="diff_"+n+".png";android.graphics.Bitmap im=asset("hud_v4_ui/"+file);
            if(im!=null)hudImage(c,file,x-im.getWidth()/2.0,y-im.getHeight()/2.0,w,u,alpha);
        }
        void drawHud(android.graphics.Canvas c) {
            if(state[126]==0)return; // Upstream panel stack allows NPC dialog/speech alone.
            org.json.JSONObject ui=hudArt();if(ui==null)return;
            float w=getWidth()/scale,u=(getHeight()/scale)/1320f;
            try {
                org.json.JSONObject base=ui.getJSONObject("base");hudImage(c,"base_full.png",base.getDouble("x"),base.getDouble("y"),w,u,255);
                org.json.JSONArray slots=ui.getJSONArray("slots"),inner=ui.getJSONArray("inner"),cross=slots.getJSONArray(5);
                boolean held=state[19+10*4]!=0,alert=state[120]>0 || state[122]!=0;
                String crossFile=alert?(held?"lvl_1.png":"lvl_0.png"):(held?"lvl_d1.png":"lvl_d0.png");
                bitmap(c,"hud_v4_ui/"+crossFile,null,hx(cross.getDouble(0),w,u),(float)cross.getDouble(1)*u,(float)(cross.getDouble(2)-cross.getDouble(0))*u,(float)(cross.getDouble(3)-cross.getDouble(1))*u,255);
                org.json.JSONObject exp=ui.getJSONObject("exp");float x0=hx(exp.getDouble("x0"),w,u),x1=hx(exp.getDouble("x1"),w,u),y=(float)exp.getDouble("y")*u,h=(float)exp.getDouble("h")*u;
                paint.setStyle(android.graphics.Paint.Style.FILL);paint.setColor(hudColor(exp.getJSONArray("track")));c.drawRect(x0,y,x1,y+h,paint);
                paint.setStyle(android.graphics.Paint.Style.STROKE);paint.setStrokeWidth(u);paint.setColor(hudColor(exp.getJSONArray("edge")));c.drawRect(x0+u/2,y+u/2,x1-u/2,y+h-u/2,paint);
                paint.setStyle(android.graphics.Paint.Style.FILL);paint.setColor(hudColor(exp.getJSONArray("fill")));c.drawRect(x0+u,y+u,x0+u+(x1-x0-2*u)*state[106]/1000,y+h-u,paint);
                paint.setColor(hudColor(exp.getJSONArray("track")));int ticks=exp.getInt("ticks");for(int t=1;t<ticks;t++){float x=x0+Math.round((x1-x0)/u*t/ticks)*u;c.drawRect(x,y+u,x+u,y+h-u,paint);}
                org.json.JSONObject vital=ui.getJSONObject("vital"),lv=ui.getJSONObject("lv"),badge=ui.getJSONObject("badge");
                hudText(c,String.format(java.util.Locale.ROOT,nativeUiText("dmHudLife"),(int)state[113],(int)state[114]),vital.getJSONObject("glyphs"),vital.getDouble("left_x"),vital.getDouble("baseline"),0,w,u);
                hudText(c,String.format(java.util.Locale.ROOT,nativeUiText("dmHudMana"),(int)state[115],(int)state[116]),vital.getJSONObject("glyphs"),vital.getDouble("right_x"),vital.getDouble("baseline"),0,w,u);
                hudText(c,String.format(java.util.Locale.ROOT,"Lv %d · %d%%",(int)state[105],(int)state[106]/10),lv.getJSONObject("glyphs"),lv.getDouble("x"),lv.getDouble("baseline"),0,w,u);
                for(int i=1;i<=4;i++)if(state[106+i]>0){org.json.JSONArray slot=slots.getJSONArray(i);hudText(c,Integer.toString((int)state[106+i]),badge.getJSONObject("glyphs"),slot.getDouble(0)+inner.getDouble(2)+badge.getDouble("dx"),slot.getDouble(1)+inner.getDouble(3)+badge.getDouble("dy"),1,w,u);}
                org.json.JSONArray first=slots.getJSONArray(0);double cx=(inner.getDouble(0)+inner.getDouble(2)+1)/2,cy=(inner.getDouble(1)+inner.getDouble(3)+1)/2;
                int players=(int)state[112];if(players>=1 && players<=8)hudDigit(c,players,first.getDouble(0)+cx,first.getDouble(1)+cy,w,u,255);
                if(state[124]!=0){org.json.JSONObject pick=ui.getJSONObject("pick");android.graphics.Bitmap tile=asset("hud_v4_ui/slot.png");for(int i=0;i<8;i++){
                    double px=pick.getDouble("center_x")+(i-3.5)*pick.getDouble("pitch"),py=pick.getDouble("y");int alpha=i+1==players?255:209;
                    if(tile!=null)hudImage(c,"slot.png",px-tile.getWidth()/2.0,py-tile.getHeight()/2.0,w,u,alpha);hudDigit(c,i+1,px,py,w,u,alpha);
                }}
            } catch(org.json.JSONException ex){android.util.Log.e("dmhost","HUD metadata mismatch",ex);hudArt=null;}
        }
        @Override protected void onDraw(android.graphics.Canvas c) {
            if(state[125]!=0){if(state[125]==1)c.drawColor(android.graphics.Color.BLACK);return;}
            if(state[0]==0)return; // Same as iOS: hidden on front-end/loading screens.
            c.save();c.scale(scale,scale);float k=state[3];boolean nav=state[1]!=0;
            drawHud(c);
            String[] navLabels=nav?new String[]{nativeUiText("dmTcNavConfirm"),nativeUiText("dmTcNavBack"),nativeUiText("dmTcNavAction"),nativeUiText("dmTcNavBind"),nativeUiText("dmTcNavNext")}:null;
            for(int i=0;i<12;i++) {
                if((((int)state[13])&(1<<i))==0 || (i>=6 && i<=10))continue;
                int at=16+i*4;float x=state[at],y=state[at+1],r=state[at+2];boolean held=state[at+3]!=0;
                if(!nav && i<=5) {
                    ring(c,x,y,r,held);
                    int glyph=64+i*8;boolean hasIcon=i<5 && state[glyph]==2;
                    if(hasIcon) {
                        String name="hud_v4_ui/sil_dm_native_skill_"+(int)state[glyph+1]+".png";
                        if(asset(name)==null)name="dm_native_skill_"+(int)state[glyph+1]+".png";
                        int sx=(int)state[glyph+2],sy=(int)state[glyph+3],sw=(int)state[glyph+4],sh=(int)state[glyph+5];
                        float size=r*1.28f;
                        bitmap(c,name,new android.graphics.Rect(sx,sy,sx+sw,sy+sh),x-size/2,y-size/2,size,size,held?255:235);
                    }
                    if(i==0 && !hasIcon) { paint.setStyle(android.graphics.Paint.Style.STROKE);paint.setColor(0xe6997847);paint.setStrokeWidth(Math.max(1.5f,r*.085f));c.drawLine(x-r*.3f,y,x+r*.3f,y,paint);c.drawLine(x,y-r*.3f,x,y+r*.3f,paint);paint.setStyle(android.graphics.Paint.Style.FILL); }
                    else if(i>=1 && i<=4) {
                        String name="hud_v4_ui/lbl_f"+(i+((int)state[2])*4)+".png";android.graphics.Bitmap label=asset(name);
                        if(label!=null){float lh=r*.36f,lw=label.getWidth()*lh/label.getHeight();bitmap(c,name,null,x-lw/2,y+r*.5f-lh/2,lw,lh,255);}
                    } else if(i==5) { // Town Portal: generic native skill atlas frame 18.
                        int frame=36+(held?1:0); float size=r*1.28f;
                        bitmap(c,"hud_v4_ui/sil_dm_native_skill_8.png",new android.graphics.Rect(frame*68,0,frame*68+65,65),x-size/2,y-size/2,size,size,held?255:235);
                    }
                } else {
                    bitmap(c,"dm_native_circle.png",null,x-r,y-r,r*2,r*2,held?153:71);
                    if(nav && i<5)caption(c,navLabels[i],x,y,(i==0?13:10)*k);
                    else if(i==11) {int frame=held?36:0;float size=r*1.24f;bitmap(c,"dm_native_map.png",new android.graphics.Rect(frame,0,frame+36,36),x-size/2,y-size/2,size,size,held?255:209);}
                }
            }
            if(!nav) {
                ring(c,state[10],state[11],state[12],false);caption(c,state[2]==0?"Ⅰ":"Ⅱ",state[10],state[11],14*k);
            }
            if(!nav && state[123]==0) {
                float x=state[4],y=state[5],r=state[6];boolean moving=state[9]!=0;
                paint.setStyle(android.graphics.Paint.Style.STROKE);paint.setStrokeWidth(k);paint.setColor(moving?0x52a88752:0x1fa88752);c.drawCircle(x,y,r,paint);paint.setStyle(android.graphics.Paint.Style.FILL);
                bitmap(c,"dm_touch_joy.png",null,x+state[7]*r-23*k,y-state[8]*r-23*k,46*k,46*k,moving?204:46);
            }
            c.restore();
        }
        void reset() { slots.clear();nativeTouch(3,0,0,0); }
        @Override public boolean onTouchEvent(android.view.MotionEvent e) {
            if(state[125]!=0){reset();return true;}
            int a=e.getActionMasked(),n=e.getActionIndex(),id=e.getPointerId(n);
            if(a==android.view.MotionEvent.ACTION_CANCEL){reset();return true;}
            if(a==android.view.MotionEvent.ACTION_DOWN || a==android.view.MotionEvent.ACTION_POINTER_DOWN) {
                int slot=0;while(slot<16 && slots.indexOfValue(slot)>=0)slot++;
                if(slot<16){slots.put(id,slot);nativeTouch(0,slot,e.getX(n)/scale,e.getY(n)/scale);}
            } else if(a==android.view.MotionEvent.ACTION_MOVE) {
                for(int i=0;i<e.getPointerCount();i++){int slot=slots.get(e.getPointerId(i),-1);if(slot>=0)nativeTouch(1,slot,e.getX(i)/scale,e.getY(i)/scale);}
            } else if(a==android.view.MotionEvent.ACTION_UP || a==android.view.MotionEvent.ACTION_POINTER_UP) {
                int slot=slots.get(id,-1);if(slot>=0){nativeTouch(1,slot,e.getX(n)/scale,e.getY(n)/scale);nativeTouch(2,slot,0,0);slots.delete(id);}
            }
            return true;
        }
    }
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(5894);
    }
    @Override public void onWindowFocusChanged(boolean focused) { super.onWindowFocusChanged(focused); if(focused) immersive(); }
    private long keyMask(int key) {
        switch (key) {
            case 19: return 1L << 13; case 20: return 1L << 15;
            case 21: return 1L << 12; case 22: return 1L << 14;
            case 23: case 66: case 96: return 1; // Enter / gamepad A
            case 111: case 97: return 2; // Escape / gamepad B
            case 99: return 4; case 100: return 8;
            case 102: return 1L << 6; case 103: return 1L << 7;
            case 104: return 1L << 8; case 105: return 1L << 9;
            case 108: return 1L << 10; case 109: return 1L << 11;
            default: return 0;
        }
    }
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        long mask = keyMask(event.getKeyCode());
        if (mask != 0 && keyboardDialog == null) {
            if (event.getAction() == android.view.KeyEvent.ACTION_DOWN) keyButtons |= mask;
            else if (event.getAction() == android.view.KeyEvent.ACTION_UP) keyButtons &= ~mask;
            publishButtons(); return true;
        }
        return super.dispatchKeyEvent(event);
    }

    public static byte[] showTextInput(byte[] title, byte[] sub, byte[] guide, byte[] initial, int maxChars) {
        MainActivity activity = active.get();
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return null;
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return null;
        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicReference<byte[]> answer = new java.util.concurrent.atomic.AtomicReference<>();
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) { done.countDown(); return; }
            try {
                java.nio.charset.Charset utf8 = java.nio.charset.StandardCharsets.UTF_8;
                android.widget.EditText input = new android.widget.EditText(activity);
                input.setSingleLine(true);
                input.setText(new String(initial, utf8));
                input.setHint(new String(guide, utf8));
                input.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(Math.min(maxChars > 0 ? maxChars : 1002, 1002))});
                android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(activity)
                    .setTitle(new String(title, utf8)).setMessage(new String(sub, utf8)).setView(input)
                    .setPositiveButton(activity.nativeUiText("dmConfirm"), (d, which) -> answer.set(input.getText().toString().getBytes(utf8)))
                    .setNegativeButton(activity.nativeUiText("dmCancel"), (d, which) -> {}).create();
                activity.keyboardDialog = dialog;
                dialog.setOnDismissListener(d -> { activity.keyboardDialog = null; done.countDown(); });
                dialog.setOnShowListener(d -> {
                    input.requestFocus();
                    dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
                });
                dialog.show();
            } catch (RuntimeException ex) { done.countDown(); }
        });
        try { done.await(); } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            activity.runOnUiThread(() -> { if (activity.keyboardDialog != null) activity.keyboardDialog.dismiss(); });
            return null;
        }
        return answer.get();
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        active = new java.lang.ref.WeakReference<>(this);
        nativeSetLanguageTag(java.util.Locale.getDefault().toLanguageTag());
        nativeRegisterUi();
        immersive();
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView render = new SurfaceView(this) {
            @Override protected void onMeasure(int widthSpec,int heightSpec) {
                int w=android.view.View.MeasureSpec.getSize(widthSpec), h=android.view.View.MeasureSpec.getSize(heightSpec);
                if(w*9>h*16) w=h*16/9; else h=w*9/16;
                setMeasuredDimension(w,h);
            }
        };
        root.addView(render,new android.widget.FrameLayout.LayoutParams(-1,-1,android.view.Gravity.CENTER));
        padView=new PadView();
        root.addView(padView,new android.widget.FrameLayout.LayoutParams(-1,-1,android.view.Gravity.CENTER));
        preparePanel=new LinearLayout(this);preparePanel.setOrientation(LinearLayout.VERTICAL);
        preparePanel.setGravity(android.view.Gravity.CENTER);preparePanel.setPadding(48,24,48,24);preparePanel.setBackgroundColor(0xff171411);
        prepareTitle=new android.widget.TextView(this);prepareTitle.setTextColor(0xffead9b8);prepareTitle.setTextSize(24);
        prepareText=new android.widget.TextView(this);prepareText.setTextColor(Color.WHITE);prepareText.setTextSize(16);prepareText.setGravity(android.view.Gravity.CENTER);
        prepareBar=new android.widget.ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);prepareBar.setIndeterminate(true);
        prepareRetry=new android.widget.Button(this);prepareRetry.setText("重试准备");prepareRetry.setOnClickListener(v -> {allowLazy=false;prepareFailed=false;startWhenReady();});
        prepareImport=new android.widget.Button(this);prepareImport.setText("选择着色器档案");prepareImport.setOnClickListener(v -> chooseShaderArchive());
        prepareContinue=new android.widget.Button(this);prepareContinue.setText("继续按需编译");prepareContinue.setOnClickListener(v -> {if(started.get())return;allowLazy=true;prepareFailed=false;startWhenReady();});
        preparePanel.addView(prepareTitle);preparePanel.addView(prepareText,new LinearLayout.LayoutParams(-1,-2));
        preparePanel.addView(prepareBar,new LinearLayout.LayoutParams(-1,-2));preparePanel.addView(prepareRetry);preparePanel.addView(prepareImport);
        preparePanel.addView(prepareContinue);
        prepareRetry.setVisibility(android.view.View.GONE);prepareImport.setVisibility(android.view.View.GONE);prepareContinue.setVisibility(android.view.View.GONE);
        prepareScroll=new ScrollView(this);prepareScroll.setFillViewport(true);prepareScroll.setBackgroundColor(0xff171411);prepareScroll.addView(preparePanel);
        root.addView(prepareScroll,new android.widget.FrameLayout.LayoutParams(-1,-1));
        if(guestRunning)prepareScroll.setVisibility(android.view.View.GONE);
        setContentView(root);
        render.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder holder) { }
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                nativeSetSurface(holder.getSurface());
                synchronized(surfaceGate){surfaceReady=true;surfaceGate.notifyAll();}
                startWhenReady();
            }
            public void surfaceDestroyed(SurfaceHolder holder) { synchronized(surfaceGate){surfaceReady=false;} nativeSetSurface(null); }
        });
    }

    private void log(final String s) {
        android.util.Log.i("dmhost", s);
        if(s.contains("失败") || s.startsWith("异常") || s.contains("不可用") || s.contains("无法") || s.startsWith("dmhost 返回"))
            prepareUi("启动未完成",s,true);
    }

    private static final String[] ASSETS = {
        "main.android.bin", "sdk.android.bin",
        "main.fixups.tsv", "sdk.fixups.tsv",
        "main.gates.tsv", "sdk.gates.tsv",
    };

    private void run() throws InterruptedException {
        File dir = getFilesDir();
        File external = getExternalFilesDir(null);
        if (external == null) { log("外部存储不可用，无法建立资源目录"); return; }
        File romfs = new File(external, "romfs");
        if (!romfs.isDirectory() && !romfs.mkdirs()) {
            log("无法建立资源目录：" + romfs.getAbsolutePath());
            return;
        }
        log("资源目录：" + romfs.getAbsolutePath());
        File manifest;
        try {
            long version = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            // Same ctrl_touch/layout overlay shipped by the upstream iOS app.
            android.content.SharedPreferences overlayPrefs=getSharedPreferences("romfs-overlay",MODE_PRIVATE);
            String[] overlayFiles={"data/.build.config","data/00-00000007.data","ctrl_touch","touch_strings.tsv"};
            File overlayRoot=new File(dir,"romfs_overlay");
            boolean prepareOverlay=overlayPrefs.getLong("apk-version",-1)!=version;
            for(String name:overlayFiles)if(!new File(overlayRoot,name).isFile())prepareOverlay=true;
            if(prepareOverlay) {
                for(String name:overlayFiles) {
                    File target=new File(overlayRoot,name),temporary=new File(overlayRoot,name+".tmp");
                    File parent=target.getParentFile();
                    if(!parent.isDirectory() && !parent.mkdirs())throw new java.io.IOException("overlay directory: "+parent);
                    try(InputStream in=getAssets().open("romfs_overlay/"+name);OutputStream out=new FileOutputStream(temporary)) {
                        byte[] buffer=new byte[65536];int count;
                        while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
                    }
                    if(!temporary.renameTo(target))throw new java.io.IOException("overlay rename: "+name);
                }
                overlayPrefs.edit().putLong("apk-version",version).apply();
            }
            manifest=prepareShaderFiles(dir,external,version);
        } catch (Exception e) { log("着色器资源准备失败：" + e); return; }
        File stamp = new File(dir, ".v1");
        if (!stamp.exists()) {
            log("首次运行：从 APK 复制模块与表…");
            for (String a : ASSETS) {
                File out = new File(dir, a);
                if (out.exists()) continue;
                try (InputStream in = getAssets().open(a);
                     OutputStream os = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                } catch (Exception e) {
                    log("复制失败 " + a + ": " + e);
                    return;
                }
                log("  已复制 " + a);
            }
            try { stamp.createNewFile(); } catch (Exception ignored) {}
        }
        if(!awaitSurface())return;
        prepareUi("正在准备 Vulkan 着色器", "预编译常用管线，其他变体首次使用时编译。首次准备可能需要几分钟。", false);
        int prepared=allowLazy?0:nativePrepareShaders(dir.getAbsolutePath(),manifest.getAbsolutePath());
        if(prepared<0){log("着色器准备失败："+preparePhase+"\n请检查档案版本和可用空间，然后重试或重新导入。");return;}
        if(prepared>0){
            prepareUi("部分管线未完成",preparePhase+"\n可以重试，或继续游戏并在首次使用时按需编译。未完成的准备不会标记为成功。",true);
            runOnUiThread(() -> {if(!destroyed)prepareContinue.setVisibility(android.view.View.VISIBLE);});
            return;
        }
        synchronized(surfaceGate){
            while(!destroyed && (!surfaceReady || !foreground))surfaceGate.wait();
            if(destroyed)return;gameEntered=true;guestRunning=true;
        }
        runOnUiThread(() -> { if(!destroyed){setPreparing(false);prepareScroll.setVisibility(android.view.View.GONE);if(foreground)getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);} });
        log("启动游戏，日志见 logcat 标签 dmhost");
        int r = nativeRun(dir.getAbsolutePath(),
                new File(dir, "main.android.bin").getAbsolutePath(),
                new File(dir, "sdk.android.bin").getAbsolutePath(),
                romfs.getAbsolutePath());
        log("dmhost 返回 " + r);
    }

    @Override protected void onResume() { super.onResume(); synchronized(surfaceGate){foreground=true;surfaceGate.notifyAll();} if(preparing || guestRunning)getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); nativeFocus(1); startWhenReady(); }
    @Override protected void onPause() { synchronized(surfaceGate){foreground=false;} getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); if(padView!=null) padView.reset(); touchButtons = keyButtons = 0; nativeStick(0,0); nativeButtons(0, true); nativeFocus(2); super.onPause(); }
    @Override protected void onStop() { nativeFocus(3); super.onStop(); }
    @Override protected void onDestroy() {
        synchronized(surfaceGate){destroyed=true;surfaceGate.notifyAll();}
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (keyboardDialog != null) keyboardDialog.dismiss();
        if (active.get() == this) active.clear();
        super.onDestroy();
    }
    private native void nativeSetLanguageTag(String languageTag);
    private native String nativeUiText(String text);
    private native void nativeRegisterUi();
    private native void nativeTouchSize(float width,float height);
    private native void nativeTouch(int action,int slot,float x,float y);
    private void pumpSaveFailure() {
        if (keyboardDialog != null || saveFailureDialog != null || isFinishing() || isDestroyed()) return;
        int err = nativeTakeSaveFailure();
        if (err == 0) return;
        android.util.Log.e("dmhost", "Save failed errno=" + err);
        saveFailureDialog = new android.app.AlertDialog.Builder(this)
            .setTitle(nativeUiText("dmSaveNoSpace"))
            .setPositiveButton(nativeUiText("dmConfirm"), null).create();
        saveFailureDialog.setOnDismissListener(d -> saveFailureDialog = null);
        saveFailureDialog.show();
    }
    private native int nativeTakeSaveFailure();
    private native void nativeTouchFrame(float[] frame);
    private native void nativeStick(int x, int y);
    private native void nativeButtons(long buttons, boolean reset);
    private native void nativeFocus(int state);

    private native void nativeSetSurface(Surface surface);

    private native int nativePrepareShaders(String root,String manifest);
    private native int nativeRun(String tables, String mainbin, String sdkbin, String romfs);

    static {
        System.loadLibrary("dmhost");
    }
}
