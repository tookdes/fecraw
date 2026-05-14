package nrup

import (
	"testing"
)

func FuzzParseClientHello(f *testing.F) {
	// 种子
	f.Add([]byte{22, 0xFE, 0xFD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50})
	f.Add([]byte{22, 0xFE, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50})
	f.Add(make([]byte, 100))

	f.Fuzz(func(t *testing.T, data []byte) {
		parseClientHello(data) // 不应panic
	})
}

func FuzzParseServerHello(f *testing.F) {
	f.Add([]byte{22, 0xFE, 0xFD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50})
	f.Add(make([]byte, 100))

	f.Fuzz(func(t *testing.T, data []byte) {
		parseServerHello(data)
	})
}

func FuzzParseQUICInitial(f *testing.F) {
	f.Add([]byte{0xC3, 0, 0, 0, 1, 8, 1, 2, 3, 4, 5, 6, 7, 8, 8, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0x40, 20, 0, 0, 0, 0, 6, 0, 0, 32})
	f.Add(make([]byte, 50))

	f.Fuzz(func(t *testing.T, data []byte) {
		parseQUICInitial(data)
	})
}
