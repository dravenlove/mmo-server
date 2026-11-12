package gateway

import "encoding/json"

// 完整对象状态持续发送；同一可靠连接已送出的表现事件不必反复重发。
func eventsAfter(raw json.RawMessage, cursor uint64) json.RawMessage {
	var source []json.RawMessage
	if len(raw) == 0 || json.Unmarshal(raw, &source) != nil {
		return raw
	}
	kept := make([]json.RawMessage, 0, len(source))
	for _, event := range source {
		var header struct {
			ID uint64 `json:"id"`
		}
		if json.Unmarshal(event, &header) != nil {
			return raw
		}
		if header.ID > cursor {
			kept = append(kept, event)
		}
	}
	out, _ := json.Marshal(kept)
	return out
}
