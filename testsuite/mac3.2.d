.! awk '{ print gensub(/([^7]*)7([^7]*)/, "\\18\\2", "g", $0) }'
