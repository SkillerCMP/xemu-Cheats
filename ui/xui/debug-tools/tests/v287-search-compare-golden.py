#!/usr/bin/env python3
# v2.87 current regression ownership.
"""Compile the real Memory Search comparison helpers and compare to a reference model."""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile
from v287_source_test_utils import extract_function, read_memory_tools_implementation


def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument("--root",default="."); ap.add_argument("--compiler",required=True); args=ap.parse_args()
    root=pathlib.Path(args.root).resolve(); text=read_memory_tools_implementation(root / "ui/xui/debug-tools")
    bodies="\n\n".join(extract_function(text,m) for m in (
        "static float raw_to_float",
        "size_t MemoryToolsWindow::ValueSize",
        "bool MemoryToolsWindow::MatchTarget",
        "bool MemoryToolsWindow::MatchPrevious",
    ))
    generated=r'''
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
class MemoryToolsWindow {
public:
 enum class ValueKind { U8=0,U16,U32,S8,S16,S32,Float32 };
 enum class NextScanMode { Exact=0,NotEqual,Changed,Unchanged,Increased,Decreased,GreaterThan,LessThan };
 ValueKind m_value_kind=ValueKind::U8;
 size_t ValueSize(ValueKind kind) const;
 bool MatchTarget(uint32_t raw,uint32_t target,NextScanMode mode) const;
 bool MatchPrevious(uint32_t current,uint32_t previous,NextScanMode mode) const;
};
__BODIES__
static uint32_t state=0x13579BDFu; static uint32_t rnd(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
static float rf(uint32_t x){float f;std::memcpy(&f,&x,4);return f;}
template<class T> static bool target_ref(T a,T b,MemoryToolsWindow::NextScanMode m){using M=MemoryToolsWindow::NextScanMode;switch(m){case M::Exact:return a==b;case M::NotEqual:return a!=b;case M::GreaterThan:return a>b;case M::LessThan:return a<b;default:return false;}}
template<class T> static bool prev_ref(T a,T b,MemoryToolsWindow::NextScanMode m){using M=MemoryToolsWindow::NextScanMode;switch(m){case M::Changed:return a!=b;case M::Unchanged:return a==b;case M::Increased:return a>b;case M::Decreased:return a<b;default:return false;}}
static bool rt(MemoryToolsWindow::ValueKind k,uint32_t a,uint32_t b,MemoryToolsWindow::NextScanMode m){using K=MemoryToolsWindow::ValueKind; if(k==K::Float32){float x=rf(a),y=rf(b);return (std::isnan(x)||std::isnan(y))?false:target_ref(x,y,m);} if(k==K::S8)return target_ref((int8_t)a,(int8_t)b,m); if(k==K::S16)return target_ref((int16_t)a,(int16_t)b,m); if(k==K::S32)return target_ref((int32_t)a,(int32_t)b,m); uint32_t mask=k==K::U8?0xffu:k==K::U16?0xffffu:0xffffffffu;return target_ref(a&mask,b&mask,m);}
static bool rp(MemoryToolsWindow::ValueKind k,uint32_t a,uint32_t b,MemoryToolsWindow::NextScanMode m){using K=MemoryToolsWindow::ValueKind; if(k==K::Float32){float x=rf(a),y=rf(b);return (std::isnan(x)||std::isnan(y))?false:prev_ref(x,y,m);} if(k==K::S8)return prev_ref((int8_t)a,(int8_t)b,m); if(k==K::S16)return prev_ref((int16_t)a,(int16_t)b,m); if(k==K::S32)return prev_ref((int32_t)a,(int32_t)b,m); uint32_t mask=k==K::U8?0xffu:k==K::U16?0xffffu:0xffffffffu;return prev_ref(a&mask,b&mask,m);}
int main(){MemoryToolsWindow w;for(int k=0;k<7;++k){w.m_value_kind=(MemoryToolsWindow::ValueKind)k;for(int m=0;m<8;++m){auto mode=(MemoryToolsWindow::NextScanMode)m;for(int i=0;i<100000;++i){uint32_t a=rnd(),b=rnd();if(w.MatchTarget(a,b,mode)!=rt(w.m_value_kind,a,b,mode))return 10+k*8+m;if(w.MatchPrevious(a,b,mode)!=rp(w.m_value_kind,a,b,mode))return 100+k*8+m;}}}const uint32_t specials[]={0x7FC00000u,0xFFC00000u,0x7F800000u,0xFF800000u,0u,0x80000000u};w.m_value_kind=MemoryToolsWindow::ValueKind::Float32;for(uint32_t a:specials)for(uint32_t b:specials)for(int m=0;m<8;++m){auto mode=(MemoryToolsWindow::NextScanMode)m;if(w.MatchTarget(a,b,mode)!=rt(w.m_value_kind,a,b,mode))return 200;if(w.MatchPrevious(a,b,mode)!=rp(w.m_value_kind,a,b,mode))return 201;}std::printf("PASS: Memory Search comparison golden (5,600,000 randomized pairs + float edge cases)\n");return 0;}
'''.replace("__BODIES__",bodies)
    with tempfile.TemporaryDirectory(prefix="xemu-search-golden-") as td:
        td=pathlib.Path(td); cpp=td/"search-golden.cpp"; exe=td/"search-golden"; cpp.write_text(generated,encoding="utf-8")
        subprocess.run([args.compiler,"-std=c++17","-O2","-Wall","-Wextra","-Werror",str(cpp),"-o",str(exe)],check=True,cwd=root)
        subprocess.run([str(exe)],check=True,cwd=root)
    return 0
if __name__=="__main__": raise SystemExit(main())
