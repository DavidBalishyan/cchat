package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/signal"
	"regexp"
	"strings"
	"syscall"
)

const (
	colorReset  = "\033[0m"
	colorRed    = "\033[31m"
	colorGreen  = "\033[32m"
	colorYellow = "\033[33m"
	colorCyan   = "\033[36m"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Println(colorRed + "Usage: go run chat_client.go <host:port>" + colorReset)
		return
	}

	serverAddr := os.Args[1]

	if !isValidHostPort(serverAddr) {
		fmt.Println(colorRed + "Invalid address format. Use <host:port>" + colorReset)
		return
	}

	conn, err := net.Dial("tcp", serverAddr)
	if err != nil {
		fmt.Println(colorRed + "Failed to connect:" + colorReset, err)
		return
	}
	defer conn.Close()

	fmt.Println(colorYellow + "Connected to " + serverAddr + colorReset)
	fmt.Println(colorYellow + "Type your message and press Enter. Use /quit to exit." + colorReset)

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigs
		conn.Close()
		os.Exit(0)
	}()

	go func() {
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			fmt.Println("\r" + colorCyan + "user: " + scanner.Text() + colorReset)
			fmt.Print(colorGreen + "You: " + colorReset)
		}
		if err := scanner.Err(); err != nil {
			fmt.Println(colorRed + "Connection error:" + colorReset, err)
		} else {
			fmt.Println(colorRed + "Server disconnected." + colorReset)
		}
		os.Exit(0)
	}()

	stdin := bufio.NewScanner(os.Stdin)
	for {
		fmt.Print(colorGreen + "You: " + colorReset)
		if !stdin.Scan() {
			break
		}
		text := strings.TrimSpace(stdin.Text())
		if text == "" {
			continue
		}
		if text == "/quit" {
			break
		}
		_, err := fmt.Fprintln(conn, text)
		if err != nil {
			fmt.Println(colorRed + "Error sending message:" + colorReset, err)
			break
		}
	}
}

func isValidHostPort(addr string) bool {
	pattern := `^([a-zA-Z0-9\.\-]+):([0-9]{1,5})$`
	matched, _ := regexp.MatchString(pattern, addr)
	return matched
}
