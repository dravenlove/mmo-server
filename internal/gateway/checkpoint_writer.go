package gateway

import "context"

type checkpointWork struct {
	route Route
	view  View
}

// 每连接串行写入；只保留一份等待中的最新快照，不让数据库阻塞战斗和状态流。
type checkpointWriter struct {
	queue chan checkpointWork
	done  chan struct{}
}

func newCheckpointWriter(server *Server) *checkpointWriter {
	w := &checkpointWriter{queue: make(chan checkpointWork, 1), done: make(chan struct{})}
	go func() {
		defer close(w.done)
		for work := range w.queue {
			server.checkpoint(context.Background(), work.route, &work.view)
		}
	}()
	return w
}

func (w *checkpointWriter) submit(route Route, view *View) {
	if view == nil || !sameRole(route.Role, view.Self) {
		return
	}
	role := view.Self
	role.Combat = append([]byte(nil), role.Combat...)
	work := checkpointWork{route: route, view: View{Self: role, Scene: role.Scene}}
	for {
		select {
		case w.queue <- work:
			return
		default:
		}
		select {
		case <-w.queue:
		default:
		}
	}
}

func (w *checkpointWriter) close() {
	close(w.queue)
	<-w.done
}
