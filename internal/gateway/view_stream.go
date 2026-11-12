package gateway

import "encoding/json"

type viewResult struct {
	route      Route
	view       View
	err        error
	generation uint64
}

func freshView(previous *View, next View) bool {
	if previous == nil || !sameRole(previous.Self, next.Self) {
		return true
	}
	if previous.WorldEpoch != "" && next.WorldEpoch != "" && previous.WorldEpoch != next.WorldEpoch {
		return true
	}
	if next.Tick < previous.Tick {
		return false
	}
	var before, after struct {
		Revision uint64 `json:"revision"`
	}
	if len(previous.Self.Combat) > 0 && len(next.Self.Combat) > 0 && json.Unmarshal(previous.Self.Combat, &before) == nil && json.Unmarshal(next.Self.Combat, &after) == nil {
		return after.Revision >= before.Revision
	}
	return true
}
