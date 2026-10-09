// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 The ASTERISC Research Lab

import java.io.*; import java.net.*; import java.util.*; import java.util.concurrent.*; import java.util.concurrent.atomic.*;
public class Stress {
    static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    static int npe(Object o) { try { return o.hashCode(); } catch (NullPointerException e) { return -1; } }
    public static void main(String[] a) throws Exception {
        long t0 = System.currentTimeMillis();
        // 1. JIT warmup + deliberate NPEs (HotSpot implements these via SIGSEGV handlers)
        if (fib(32) != 2178309) throw new Error("fib");
        int nulls = 0; for (int i = 0; i < 200000; i++) if (npe(i % 3 == 0 ? null : "x") == -1) nulls++;
        if (nulls != 66667) throw new Error("npe " + nulls);
        // 2. GC pressure across threads
        ExecutorService ex = Executors.newFixedThreadPool(8); List<Future<Long>> fs = new ArrayList<>();
        for (int t = 0; t < 8; t++) fs.add(ex.submit(() -> { long s = 0; Random r = new Random(); List<byte[]> keep = new ArrayList<>();
            for (int i = 0; i < 200000; i++) { byte[] b = new byte[1024 + r.nextInt(4096)]; b[0] = 1; s += b[0]; if (i % 50 == 0) keep.add(b); if (keep.size() > 400) keep.clear(); } return s; }));
        long tot = 0; for (Future<Long> f : fs) tot += f.get(); if (tot != 1600000) throw new Error("gc " + tot);
        // 3. contended concurrency
        ConcurrentHashMap<Integer, AtomicInteger> m = new ConcurrentHashMap<>(); CountDownLatch cd = new CountDownLatch(8);
        for (int t = 0; t < 8; t++) ex.execute(() -> { for (int i = 0; i < 200000; i++) m.computeIfAbsent(i % 100, k -> new AtomicInteger()).incrementAndGet(); cd.countDown(); });
        cd.await(); int c = 0; for (AtomicInteger v : m.values()) c += v.get(); if (c != 1600000) throw new Error("chm " + c);
        // 4. socket echo
        ServerSocket ss = new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
        ex.execute(() -> { try { while (true) { Socket s = ss.accept(); ex.execute(() -> { try (s; InputStream in = s.getInputStream(); OutputStream out = s.getOutputStream()) { byte[] buf = new byte[4096]; int n; while ((n = in.read(buf)) > 0) out.write(buf, 0, n); } catch (IOException e) {} }); } } catch (IOException e) {} });
        for (int i = 0; i < 100; i++) try (Socket s = new Socket(InetAddress.getLoopbackAddress(), ss.getLocalPort())) {
            byte[] msg = new byte[2000]; Arrays.fill(msg, (byte) i); s.getOutputStream().write(msg); byte[] back = s.getInputStream().readNBytes(2000); if (!Arrays.equals(msg, back)) throw new Error("echo"); }
        ss.close(); ex.shutdownNow();
        // 5. files + process exit code
        File f = File.createTempFile("strs", ".bin"); try (FileOutputStream o = new FileOutputStream(f)) { o.write(new byte[1 << 20]); } if (f.length() != (1 << 20)) throw new Error("file"); f.delete();
        System.out.println("STRESS OK " + (System.currentTimeMillis() - t0) + " ms");
        System.exit(0);
    }
}
