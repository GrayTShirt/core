package main

import (
	"fmt"
	"time"

	"github.com/jhunt/go-firehose"
)

type MetricType int

const (
	NS = 1000000000

	SampleMetric MetricType = iota
	CounterMetric
	ContainerMetric
)

type Metric struct {
	Type      MetricType
	Timestamp int64
	Uint      uint64
	Float     float64
}

type Nozzle struct {
	Prefix string

	metrics map[string]Metric
}

func NewNozzle() *Nozzle {
	nozzle := &Nozzle{}
	nozzle.Reset()
	return nozzle
}

func (nozzle *Nozzle) Reset() {
	nozzle.metrics = make(map[string]Metric)
}

func (nozzle *Nozzle) Configure(c firehose.Config) {
	nozzle.Prefix = c.Prefix
}

func (nozzle *Nozzle) Track(e firehose.Event) {
	if e.GetOrigin() == "MetronAgent" {
		return
	}

	var k string
	switch e.Type() {
	case firehose.CounterEvent:
		m := e.GetCounterEvent()
		k = fmt.Sprintf("%s%s:%s:%s", nozzle.Prefix, e.GetDeployment(), e.GetOrigin(), m.GetName())

		if v, ok := nozzle.metrics[k]; ok && v.Type == CounterMetric {
			v.Uint = m.GetTotal()
			v.Timestamp = e.GetTimestamp()
		} else {
			nozzle.metrics[k] = Metric{
				Type:      CounterMetric,
				Timestamp: e.GetTimestamp(),
				Uint:      m.GetTotal(),
			}
		}

	case firehose.ValueMetric:
		m := e.GetValueMetric()
		/* NOTE: sometimes a "ValueMetric" is actually a counter, in disguise... */
		u := m.GetUnit()
		if u == "count" {
			k = fmt.Sprintf("%s%s:%s:%s", nozzle.Prefix, e.GetDeployment(), e.GetOrigin(), m.GetName())
			if v, ok := nozzle.metrics[k]; ok && v.Type == CounterMetric {
				v.Uint = uint64(m.GetValue())
				v.Timestamp = e.GetTimestamp()
			} else {
				nozzle.metrics[k] = Metric{
					Type:      CounterMetric,
					Timestamp: e.GetTimestamp(),
					Uint:      uint64(m.GetValue()),
				}
			}
		} else {
			if u == "gauge" {
				k = fmt.Sprintf("%s%s:%s:%s", nozzle.Prefix, e.GetDeployment(), e.GetOrigin(), m.GetName())
			} else {
				k = fmt.Sprintf("%s%s:%s:%s.%s", nozzle.Prefix, e.GetDeployment(), e.GetOrigin(), m.GetName(), m.GetUnit())
			}

			if v, ok := nozzle.metrics[k]; ok && v.Type == SampleMetric {
				v.Float = m.GetValue()
				v.Timestamp = e.GetTimestamp()
			} else {
				nozzle.metrics[k] = Metric{
					Type:      SampleMetric,
					Timestamp: e.GetTimestamp(),
					Float:     m.GetValue(),
				}
			}
		}

	case firehose.ContainerMetric:
		m := e.GetContainerMetric()
		k = fmt.Sprintf("%s%s:%s:%s:%s:", nozzle.Prefix, e.GetDeployment(), e.GetOrigin(), m.GetApplicationId(), m.GetInstanceIndex())

		if v, ok := nozzle.metrics[k+"cpu"]; ok && v.Type == SampleMetric {
			v.Float = m.GetCpuPercentage()
		} else {
			nozzle.metrics[k+"cpu"] = Metric{
				Type:      SampleMetric,
				Timestamp: e.GetTimestamp(),
				Float:     m.GetCpuPercentage(),
			}
		}

		if v, ok := nozzle.metrics[k+"mem"]; ok && v.Type == SampleMetric {
			v.Uint = m.GetMemoryBytes()
			v.Timestamp = e.GetTimestamp()
		} else {
			nozzle.metrics[k+"mem"] = Metric{
				Type:      SampleMetric,
				Timestamp: e.GetTimestamp(),
				Uint:      m.GetMemoryBytes(),
			}
		}

		if v, ok := nozzle.metrics[k+"disk"]; ok && v.Type == SampleMetric {
			v.Uint = m.GetDiskBytes()
			v.Timestamp = e.GetTimestamp()
		} else {
			nozzle.metrics[k+"disk"] = Metric{
				Type:      SampleMetric,
				Timestamp: e.GetTimestamp(),
				Uint:      m.GetDiskBytes(),
			}
		}
	}
}

func (nozzle *Nozzle) Flush() error {
	n := 0
	for name, metric := range nozzle.metrics {
		n++
		if metric.Type == CounterMetric {
			fmt.Printf("COUNTER %d %s %d\n", metric.Timestamp / NS, name, metric.Uint)
		}
		if metric.Type == SampleMetric {
			fmt.Printf("SAMPLE  %d %s %f\n", metric.Timestamp / NS, name, metric.Float)
		}
		if metric.Type == ContainerMetric {
			fmt.Printf("SAMPLE  %d %s %d\n", metric.Timestamp / NS, name, metric.Uint)
		}
	}
	fmt.Printf("SAMPLE  %d %scf-bolo-nozzle:metrics %d\n", time.Now().Unix(), nozzle.Prefix, n)
	nozzle.Reset()
	return nil
}

func (nozzle *Nozzle) SlowConsumer() {
}
