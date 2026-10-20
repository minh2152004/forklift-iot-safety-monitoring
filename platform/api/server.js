const express = require("express")
const cors = require("cors")
const { InfluxDB } = require("@influxdata/influxdb-client")

const app = express()
app.use(cors())

const url = process.env.INFLUXDB_URL
const token = process.env.DOCKER_INFLUXDB_INIT_ADMIN_TOKEN
const org = process.env.DOCKER_INFLUXDB_INIT_ORG
const bucket = process.env.DOCKER_INFLUXDB_INIT_BUCKET


const influx = new InfluxDB({ url, token })
const queryApi = influx.getQueryApi(org)

app.get("/data", async (req, res) => {

const range = req.query.range || 15   // default 15 minutes

const query = `
from(bucket:"${bucket}")
  |> range(start: -${range}m)
  |> filter(fn:(r)=> r._measurement == "iotsensor")
  |> sort(columns: ["_time"])
`

const results = []

queryApi.queryRows(query, {

next(row, meta) {
const obj = meta.toObject(row)
results.push(obj)
},

error(error) {
console.error(error)
res.status(500).send(error)
},

complete() {
res.json(results)
}

})

})
app.get('/zone', async (req, res) => {
    const query = `
from(bucket:"${bucket}")
  |> range(start: -5m)
  |> filter(fn:(r) => r._measurement == "location")
  |> last()
`

    const rows = []

    queryApi.queryRows(query, {
        next(row, meta) {
            rows.push(meta.toObject(row))
        },
        error(error) {
            console.error("Zone query error:", error)
            res.status(500).json({ error: "query failed" })
        },
        complete() {
            if (rows.length === 0) {
                return res.json({
                    zone: "router offline",
                    rssi: null,
                    minor: null,
                    ts: null,
                    stale: true
                })
            }

            // InfluxDB returns one row per field — pivot them into one object
            const point = {}
            let latestTime = null
            for (const r of rows) {
                point[r._field] = r._value
                if (!latestTime || r._time > latestTime) latestTime = r._time
            }

            const ageSec = (Date.now() - new Date(latestTime).getTime()) / 1000
            const stale = ageSec > 30

            res.json({
                zone:  stale ? "router offline" : (point.zone_text || "unknown"),
                rssi:  stale ? null : (point.rssi ?? null),
                minor: stale ? null : (point.minor ?? null),
                ts:    latestTime,
                stale: stale,
                age_sec: Math.round(ageSec)
            })
        }
    })
})
app.listen(5000, () => {
console.log("API running on port 5000")
})