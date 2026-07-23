#pragma once
// CAVES_V3 — vanilla-1.21.1-faithful carvers (NOT noise).
// Direct port of Mojang deobfuscated CaveWorldCarver + CanyonWorldCarver and the
// NoiseBasedChunkGenerator.applyCarvers driver:
//   * for the target chunk, iterate nearby neighbour chunks in [-4..4] (fast tuned mode),
//   * seed a Java-LCG random with setLargeFeatureSeed(seed+carverIdx, ncx, ncz),
//   * run each carver's isStartChunk() probability, then carve() into THIS chunk.
// Tuned carvers: [0]=cave (p=0.055), [1]=cave_extra (p=0.025), [2]=canyon (p=0.004).
// This produces real winding tunnels / rooms / ravines that cross chunk borders,
// which is what actually reads as "vanilla" (the old ridged-noise sponge did not).
#include "chunk.hpp"
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

namespace nc::world::caves {

static constexpr double PI_ = 3.14159265358979323846;

// ---- Java java.util.Random-compatible source (for vanilla-identical distributions) ----
struct JRand {
    uint64_t seed=0;
    static constexpr uint64_t MUL=0x5DEECE66Dull, ADD=0xBull, MASK=(1ull<<48)-1;
    void setSeed(int64_t s){ seed=((uint64_t)s ^ MUL)&MASK; }
    int32_t next(int bits){ seed=(seed*MUL+ADD)&MASK; return (int32_t)(uint32_t)(seed>>(48-bits)); }
    int32_t nextInt(){ return next(32); }
    int32_t nextInt(int bound){
        if(bound<=0) return 0;
        if((bound&(bound-1))==0) return (int32_t)(((int64_t)bound*(int64_t)next(31))>>31);
        int bits,val;
        do{ bits=next(31); val=bits%bound; }while(bits-val+(bound-1)<0);
        return val;
    }
    int64_t nextLong(){ int64_t hi=next(32); int64_t lo=next(32); return (hi<<32)+lo; }
    float nextFloat(){ return next(24)/(float)(1<<24); }
    void setLargeFeatureSeed(int64_t s,int cx,int cz){ setSeed((int64_t)cx*341873128712LL + (int64_t)cz*132897987541LL + s); }
};
inline float uf(JRand&r,float a,float b){ return a + r.nextFloat()*(b-a); }              // UniformFloat
inline int   ui(JRand&r,int a,int b){ return a + r.nextInt(b-a+1); }                       // UniformHeight (inclusive)
inline float trapezoid(JRand&r,float mn,float mx,float plateau){ float f=mx-mn; float f2=(f-plateau)/2.0f; float f3=f-f2; return mn + r.nextFloat()*f3 + r.nextFloat()*f2; }

// ---- carve target / context ----
struct Ctx {
    ChunkColumn& chunk; int cx, cz; i32 AIR, WATER, LAVA;
    static constexpr int MIN_GEN_Y=-64, GEN_DEPTH=384, LAVA_LEVEL=-60, SEA_LEVEL=63; // fewer exposed lava pools
};

inline bool carveReplaceable(i32 id){
    switch(id){
        case 1: case 2: case 4: case 6:            // stone/granite/diorite/andesite
        case 9: case 10: case 11: case 13:         // grass_block/dirt/coarse_dirt/podzol
        case 112: case 117: case 118:              // sand/red_sand/gravel
        case 21081: case 24905: case 24902:        // tuff/deepslate/rooted_dirt
            return true;
        default: return false;
    }
}

// carve one block in world coords into the current chunk
inline void carveBlock(Ctx&C,int wx,int y,int wz){
    if(wx<C.cx*16||wx>C.cx*16+15||wz<C.cz*16||wz>C.cz*16+15) return;
    if(y<=C.MIN_GEN_Y||y>=C.MIN_GEN_Y+C.GEN_DEPTH) return;
    i32 cur=C.chunk.getBlock(wx,y,wz);
    if(cur==C.AIR||cur==C.WATER||cur==C.LAVA) return;
    if(!carveReplaceable(cur)) return;
    if(C.chunk.getBlock(wx,y+1,wz)==C.WATER) return;   // keep ocean/lake floors intact
    C.chunk.setBlock(wx,y,wz, y<=C.LAVA_LEVEL?C.LAVA:C.AIR);
}

inline bool canReach(int cx,int cz,double x,double z,int i,int len,float thickness){
    double midX=cx*16+8, midZ=cz*16+8;
    double dx=x-midX, dz=z-midZ, dl=(double)(len-i);
    double lim=thickness+2.0+16.0;
    return dx*dx + dz*dz - dl*dl <= lim*lim;
}

// shared ellipsoid bounds walker; `mode`: 0=cave (sphere+floorLevel), 1=canyon (weighted)
inline void carveEllipsoid(Ctx&C,double d,double d2,double d3,double horiz,double vert,
                           int mode,double floorLevel,const std::vector<float>* wf){
    double midX=C.cx*16+8, midZ=C.cz*16+8;
    double reach=16.0+horiz*2.0;
    if(std::fabs(d-midX)>reach || std::fabs(d3-midZ)>reach) return;
    int minBX=C.cx*16, minBZ=C.cz*16;
    int n3=std::max((int)std::floor(d-horiz)-minBX-1,0);
    int n4=std::min((int)std::floor(d+horiz)-minBX,15);
    int n5=std::max((int)std::floor(d2-vert)-1,C.MIN_GEN_Y+1);
    int n7=std::min((int)std::floor(d2+vert)+1,C.MIN_GEN_Y+C.GEN_DEPTH-1-7);
    int n8=std::max((int)std::floor(d3-horiz)-minBZ-1,0);
    int n9=std::min((int)std::floor(d3+horiz)-minBZ,15);
    for(int i=n3;i<=n4;++i){
        int wx=minBX+i; double dnx=((double)wx+0.5-d)/horiz;
        for(int j=n8;j<=n9;++j){
            int wz=minBZ+j; double dnz=((double)wz+0.5-d3)/horiz;
            if(dnx*dnx+dnz*dnz>=1.0) continue;
            for(int k=n7;k>n5;--k){
                double dny=((double)k-0.5-d2)/vert;
                bool skip;
                if(mode==0) skip=(dny<=floorLevel)||(dnx*dnx+dny*dny+dnz*dnz>=1.0);
                else{ int idx=k-C.MIN_GEN_Y-1; float w=(idx>=0&&idx<(int)wf->size())?(*wf)[idx]:1.0f; skip=((dnx*dnx+dnz*dnz)*(double)w + dny*dny/6.0)>=1.0; }
                if(skip) continue;
                carveBlock(C,wx,k,wz);
            }
        }
    }
}

// ---- CaveWorldCarver ----
inline float caveThickness(JRand&r){
    float f=r.nextFloat()*2.0f + r.nextFloat();
    if(r.nextInt(10)==0) f*= r.nextFloat()*r.nextFloat()*3.0f + 1.0f;
    return f;
}
inline void caveTunnel(Ctx&C,int64_t l,double d,double d2,double d3,double horizMult,double vertMult,
                       float thickness,float yaw,float pitch,int start,int len,double floorLevel){
    JRand r; r.setSeed(l);
    int n3=r.nextInt(len/2)+len/4;
    bool wide=r.nextInt(6)==0;
    float f4=0.0f,f5=0.0f;
    for(int i=start;i<len;++i){
        double d7=1.5 + std::sin(PI_*(double)i/(double)len)*thickness;
        double d8=d7*1.0; // yScale = getYScale() = 1.0
        float f6=std::cos(pitch);
        d  += std::cos(yaw)*f6;
        d2 += std::sin(pitch);
        d3 += std::sin(yaw)*f6;
        pitch *= wide?0.92f:0.7f;
        pitch += f5*0.1f; yaw += f4*0.1f;
        f5*=0.9f; f4*=0.75f;
        f5 += (r.nextFloat()-r.nextFloat())*r.nextFloat()*2.0f;
        f4 += (r.nextFloat()-r.nextFloat())*r.nextFloat()*4.0f;
        if(i==n3 && thickness>1.0f){
            caveTunnel(C,r.nextLong(),d,d2,d3,horizMult,vertMult,r.nextFloat()*0.5f+0.5f,yaw-(float)PI_/2.0f,pitch/3.0f,i,len,floorLevel);
            caveTunnel(C,r.nextLong(),d,d2,d3,horizMult,vertMult,r.nextFloat()*0.5f+0.5f,yaw+(float)PI_/2.0f,pitch/3.0f,i,len,floorLevel);
            return;
        }
        if(r.nextInt(4)==0) continue;
        if(!canReach(C.cx,C.cz,d,d3,i,len,thickness)) return;
        carveEllipsoid(C,d,d2,d3,d7*horizMult,d8*vertMult,0,floorLevel,nullptr);
    }
}
// ---- CanyonWorldCarver ----
inline double canyonVertRadius(JRand&r,double d,int len,int i){
    float f3=1.0f - std::fabs(0.5f-(float)i/(float)len)*2.0f;
    float f4=1.0f + 0.0f*f3; // verticalRadiusDefaultFactor=1, centerFactor=0
    return (double)f4 * d * (double)uf(r,0.75f,1.0f);
}
inline void canyonDoCarve(Ctx&C,int64_t l,double d,double d2,double d3,float thickness,
                          float yaw,float pitch,int start,int len,double yScale){
    JRand r; r.setSeed(l);
    std::vector<float> wf(C.GEN_DEPTH,1.0f);
    { float f=1.0f; for(int i=0;i<C.GEN_DEPTH;++i){ if(i==0||r.nextInt(3)==0) f=1.0f + r.nextFloat()*r.nextFloat(); wf[i]=f*f; } }
    float f4=0.0f,f5=0.0f;
    for(int i=start;i<len;++i){
        double d8=1.5 + std::sin((float)i*(float)PI_/(float)len)*thickness;
        double d9=d8*yScale;
        d8 *= (double)uf(r,0.75f,1.0f);              // horizontalRadiusFactor
        d9 = canyonVertRadius(r,d9,len,i);
        float f6=std::cos(pitch), f7=std::sin(pitch);
        d  += std::cos(yaw)*f6;
        d2 += f7;
        d3 += std::sin(yaw)*f6;
        pitch*=0.7f; pitch+=f5*0.05f; yaw+=f4*0.05f;
        f5*=0.8f; f4*=0.5f;
        f5 += (r.nextFloat()-r.nextFloat())*r.nextFloat()*2.0f;
        f4 += (r.nextFloat()-r.nextFloat())*r.nextFloat()*4.0f;
        if(r.nextInt(4)==0) continue;
        if(!canReach(C.cx,C.cz,d,d3,i,len,thickness)) return;
        carveEllipsoid(C,d,d2,d3,d8,d9,1,0.0,&wf);
    }
}

// ---- driver: applyCarvers over nearby neighbours [-4..4] ----
inline void carve(ChunkColumn& chunk,i32 cx,i32 cz,u64 seed,i32 air,i32 water,i32 lava){
    Ctx C{chunk,cx,cz,air,water,lava};
    int64_t ws=(int64_t)seed;
    for(int di=-4;di<=4;++di) for(int dj=-4;dj<=4;++dj){
        int ncx=cx+di, ncz=cz+dj;
        JRand r;
        // carver 0: cave (p=0.055), y in aboveBottom(8)=-56 .. absolute(180)
        r.setLargeFeatureSeed(ws+0,ncx,ncz);
        if(r.nextFloat()<=0.055f){
            int len=112;
            int starts=r.nextInt(r.nextInt(r.nextInt(15)+1)+1);
            for(int s=0;s<starts;++s){
                double d =(double)(ncx*16 + r.nextInt(16));
                double d5=(double)ui(r,-56,180);
                double d6=(double)(ncz*16 + r.nextInt(16));
                double d7=uf(r,0.7f,1.4f);   // horizontalRadiusMultiplier
                double d8=uf(r,0.8f,1.3f);   // verticalRadiusMultiplier
                double d9=uf(r,-1.0f,-0.4f); // floorLevel
                int n4=1;
                if(r.nextInt(4)==0){
                    double d10=uf(r,0.1f,0.9f);           // yScale for room
                    float f=1.0f + r.nextFloat()*6.0f;
                    double rr=1.5 + std::sin(1.5707964f)*f; // = 1.5 + f
                    carveEllipsoid(C,d+1.0,d5,d6,rr,rr*d10,0,d9,nullptr);
                    n4+=r.nextInt(4);
                }
                for(int j=0;j<n4;++j){
                    float f2=r.nextFloat()*(float)PI_*2.0f;
                    float f=(r.nextFloat()-0.5f)/4.0f;
                    float f3=caveThickness(r);
                    int n5=len - r.nextInt(len/4);
                    caveTunnel(C,r.nextLong(),d,d5,d6,d7,d8,f3,f2,f,0,n5,d9);
                }
            }
        }
        // carver 1: cave_extra_underground (p=0.025), y in -56 .. absolute(47)
        r.setLargeFeatureSeed(ws+1,ncx,ncz);
        if(r.nextFloat()<=0.025f){
            int len=112;
            int starts=r.nextInt(r.nextInt(r.nextInt(15)+1)+1);
            for(int s=0;s<starts;++s){
                double d =(double)(ncx*16 + r.nextInt(16));
                double d5=(double)ui(r,-56,47);
                double d6=(double)(ncz*16 + r.nextInt(16));
                double d7=uf(r,0.7f,1.4f);
                double d8=uf(r,0.8f,1.3f);
                double d9=uf(r,-1.0f,-0.4f);
                int n4=1;
                if(r.nextInt(4)==0){
                    double d10=uf(r,0.1f,0.9f);
                    float f=1.0f + r.nextFloat()*6.0f;
                    double rr=1.5 + f;
                    carveEllipsoid(C,d+1.0,d5,d6,rr,rr*d10,0,d9,nullptr);
                    n4+=r.nextInt(4);
                }
                for(int j=0;j<n4;++j){
                    float f2=r.nextFloat()*(float)PI_*2.0f;
                    float f=(r.nextFloat()-0.5f)/4.0f;
                    float f3=caveThickness(r);
                    int n5=len - r.nextInt(len/4);
                    caveTunnel(C,r.nextLong(),d,d5,d6,d7,d8,f3,f2,f,0,n5,d9);
                }
            }
        }
        // carver 2: canyon (p=0.004), y in absolute(10)..absolute(67)
        r.setLargeFeatureSeed(ws+2,ncx,ncz);
        if(r.nextFloat()<=0.004f){
            int len=112;
            double d =(double)(ncx*16 + r.nextInt(16));
            int n2y=ui(r,10,67);
            double d2=(double)(ncz*16 + r.nextInt(16));
            float yaw=r.nextFloat()*(float)PI_*2.0f;
            float pitch=uf(r,-0.125f,0.125f);           // verticalRotation
            double yScale=3.0;                          // ConstantFloat.of(3.0)
            float thickness=trapezoid(r,0.0f,6.0f,2.0f);
            int n3=(int)((float)len * uf(r,0.75f,1.0f)); // distanceFactor
            canyonDoCarve(C,r.nextLong(),d,(double)n2y,d2,thickness,yaw,pitch,0,n3,yScale);
        }
    }
}

} // namespace nc::world::caves
