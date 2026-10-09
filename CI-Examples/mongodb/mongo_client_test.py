import sys, time, threading
from pymongo import MongoClient, ASCENDING
port = int(sys.argv[1]) if len(sys.argv) > 1 else 27017
t0 = time.time()
c = MongoClient("mongodb://127.0.0.1:%d" % port, serverSelectionTimeoutMS=60000)
db = c.mydatabase
# 1. the example's insert.js / fetch.js
db.products.drop()
db.products.insert_many([{"item": "card", "quantity": 25}, {"item": "pen", "quantity": 30}, {"item": "lamp", "quantity": 20}])
assert sorted(d["item"] for d in db.products.find()) == ["card", "lamp", "pen"]
# 2. bulk insert + index + queries + aggregation
big = db.big; big.drop()
big.insert_many([{"i": i, "g": i % 17, "s": "x" * 100, "v": i * 0.5} for i in range(50000)])
big.create_index([("i", ASCENDING)]); big.create_index("g")
assert big.count_documents({}) == 50000
assert big.find_one({"i": 31337})["v"] == 31337 * 0.5
agg = {d["_id"]: d["n"] for d in big.aggregate([{"$group": {"_id": "$g", "n": {"$sum": 1}}}])}
assert len(agg) == 17 and sum(agg.values()) == 50000
assert big.count_documents({"i": {"$gte": 1000, "$lt": 2000}}) == 1000
for k in range(8): db.drop_collection('w%d' % k)
# 3. concurrent clients (threads) doing writes + reads + updates
errs = []
def worker(k):
    try:
        cc = MongoClient("mongodb://127.0.0.1:%d" % port); col = cc.mydatabase["w%d" % k]
        for i in range(500):
            col.insert_one({"k": k, "i": i}); col.update_one({"i": i}, {"$set": {"u": 1}})
        assert col.count_documents({"u": 1}) == 500
    except Exception as e:
        errs.append(repr(e))
ts = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
[t.start() for t in ts]; [t.join() for t in ts]
assert not errs, errs
# 4. durability path: fsync-ish command and stats
assert c.admin.command("ping")["ok"] == 1
assert db.command("dbstats")["ok"] == 1
print("MONGO OK %.1fs" % (time.time() - t0))
