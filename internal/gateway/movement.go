package gateway

import (
	"context"
	"errors"
	"log"
	"sync"
	"sync/atomic"
	"time"
)

const MaxMessageBytes = 65535

type connectionRoute struct {
	Route      Route
	ValidUntil time.Time
}

func sameRole(a, b Role) bool {
	return a.ID == b.ID && a.Scene == b.Scene && a.Owner == b.Owner && a.Session == b.Session
}

func (s *Server) clientRole(role Role) Role {
	if s.cfg.Profile == "movement" || s.cfg.Profile == "elemental" {
		role.Combat = nil
	}
	return role
}

func (s *Server) clientView(view View) View {
	if s.cfg.Profile == "movement" {
		view.Self = s.clientRole(view.Self)
		view.Players = append([]Role(nil), view.Players...)
		for i := range view.Players {
			view.Players[i] = s.clientRole(view.Players[i])
		}
		view.Mobs, view.Skills, view.Events = nil, nil, nil
		view.Projectiles, view.Walls = nil, nil
		view.WorldEpoch, view.AbilityRules = "", ""
		view.PVPEnabled = nil
		view.ProjectilesTruncated, view.EventsTruncated = false, false
		view.EventCursor = 0
		view.Features, view.Battle = nil, nil
	}
	if s.cfg.Profile == "elemental" || (s.cfg.Profile != "movement" && len(view.Battle) > 0) {
		return elementalClientView(view)
	}
	return view
}

// 一个有界移动队列保留请求顺序。视图和数据库调用不占用此执行通道。
// 多条方向输入在同一场景 Tick 中逐条校验、接收；每条请求仍有独立回执。
func (s *Server) moveWorker(ctx context.Context, queue <-chan Request, published *atomic.Pointer[connectionRoute], barrier *sync.Mutex, write func(Response) bool) {
	for {
		var first Request
		select {
		case <-ctx.Done():
			return
		case first = <-queue:
		}
		batch := []Request{first}
		for len(batch) < 4 {
			select {
			case next := <-queue:
				batch = append(batch, next)
			default:
				goto ready
			}
		}
	ready:
		barrier.Lock()
		current := published.Load()
		started := time.Now()
		var failure error
		var results []Response
		var rejected []Response
		if s.cfg.Profile == "elemental" || s.cfg.Profile == "skill_lab" {
			kept := batch[:0]
			for _, request := range batch {
				if request.receivedRoute == nil {
					rejected = append(rejected, Response{ID: request.ID, Op: "move", Error: "login required"})
				} else if current == nil || !sameRole(request.receivedRoute.Route.Role, current.Route.Role) {
					rejected = append(rejected, Response{ID: request.ID, Op: "move", OK: true, Data: elementalRejected("scene_changed")})
				} else {
					kept = append(kept, request)
				}
			}
			batch = kept
			if len(batch) == 0 {
				barrier.Unlock()
				for _, response := range rejected {
					if !write(response) {
						return
					}
				}
				continue
			}
		}
		if current == nil {
			failure = errors.New("login required")
		} else if started.After(current.ValidUntil) {
			failure = errors.New("session verification expired; reconnect")
		} else if len(batch) == 1 {
			request := batch[0]
			body := ref(current.Route.Role)
			body["seq"], body["dx"], body["dy"], body["trace_id"] = request.Seq, request.DX, request.DY, request.ID
			var data any
			failure = s.sceneCall(ctx, current.Route, "/v1/move", body, &data)
			results = []Response{{OK: failure == nil, Data: data}}
		} else {
			body := ref(current.Route.Role)
			inputs := make([]map[string]any, 0, len(batch))
			for _, request := range batch {
				inputs = append(inputs, map[string]any{"seq": request.Seq, "dx": request.DX, "dy": request.DY, "trace_id": request.ID})
			}
			body["inputs"] = inputs
			var out struct {
				Results []Response `json:"results"`
			}
			failure = s.sceneCall(ctx, current.Route, "/v1/move_batch", body, &out)
			if failure == nil && len(out.Results) != len(batch) {
				failure = errors.New("invalid movement batch response")
			}
			results = out.Results
		}
		latest := published.Load()
		if failure == nil && (latest == nil || !sameRole(current.Route.Role, latest.Route.Role)) {
			failure = errors.New("scene or session changed while applying input")
		}
		barrier.Unlock()
		if ctx.Err() != nil {
			return
		}
		for _, response := range rejected {
			if !write(response) {
				return
			}
		}
		for i, request := range batch {
			response := Response{ID: request.ID, Op: "move"}
			if failure != nil {
				response.Error = failure.Error()
			} else {
				response.OK, response.Data, response.Error = results[i].OK, results[i].Data, results[i].Error
			}
			if !response.OK {
				response.Data = nil
				if response.Error == "" {
					response.Error = "movement was rejected"
				}
			}
			writing := time.Now()
			if !write(response) {
				return
			}
			if s.cfg.TraceMovement {
				log.Printf("movement id=%q seq=%d accepted=%t queue_ms=%d service_ms=%d write_ms=%d", request.ID, request.Seq, response.OK, started.Sub(request.receivedAt).Milliseconds(), writing.Sub(started).Milliseconds(), time.Since(writing).Milliseconds())
			}
		}
	}
}
