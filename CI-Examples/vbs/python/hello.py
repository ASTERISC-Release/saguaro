import hashlib
import platform
import sys
import threading

print(f"Python {sys.version.split()[0]} on {platform.system()} {platform.machine()}")
print("sha256(b'gramine') =", hashlib.sha256(b"gramine").hexdigest())
results = []
threads = [threading.Thread(target=lambda n=n: results.append(sum(range(n * 100000))))
           for n in range(1, 4)]
for t in threads:
    t.start()
for t in threads:
    t.join()
print("threads:", sorted(results))
