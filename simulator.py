class Cache:
    def __init__(self, size, depth):
        self.data = [[None] * size for _ in range(depth)]
        self.depth = depth
        self.size = size
        self.register = [None] * size
    
    def receive_data(self, row, values):
        if row < len(self.data):
            self.data[row] = values
    
    def send_data(self):
        if self.data:
            self.register = self.data.pop(0)

    
    def send_register(self, col):
        if self.register:
            return self.register[col]
        else:
            return 0    
    def send_registers(self):
        if self.register:
            return self.register

class ProcessingElement:
    def __init__(self, row, col):
        self.row = row
        self.col = col
        self.registers = {'src1': 0, 'src2': 0, 'compute_result': 0, 'received_result': 0, 'src1Send' : 0, 'src2Send' : 0, 'resultSend' : 0}
        self.ready_to_compute = False
    
    def receive_src1(self, value):
        self.registers['src1'] = value
    
    def receive_src2(self, value):
        self.registers['src2'] = value
    
    def receive_compute_result(self, value):
        self.registers['received_result'] = value
        
    def compute(self):
        if self.registers['src1'] is not 0 and self.registers['src2'] is not 0:
            self.registers['compute_result'] = self.registers['src1'] * self.registers['src2'] + self.registers['received_result']
            print(f"PE [{self.row}][{self.col}] computing: {self.registers['src1']} * {self.registers['src2']} + {self.registers['received_result']} = {self.registers['compute_result']}")
        else:
            self.registers['compute_result'] = self.registers['received_result']
            print(f"PE [{self.row}][{self.col}] got: {self.registers['compute_result']} NO COMPUTING!!!")
    
    def send_src1(self):
        self.registers['src1Send'] = self.registers['src1']

    def send_src2(self):
        self.registers['src2Send'] = self.registers['src2']

    def send_compute_result(self):
        self.registers['resultSend'] = self.registers['compute_result']
    
    def get_src1Send(self):
        return self.registers['src1Send']
    
    def get_src2Send(self):
        return self.registers['src2Send']
    
    def get_src1(self):
        return self.registers['src1']
    
    def get_src2(self):
        return self.registers['src2']

    def get_received_result(self):
        return self.registers['received_result']
    
    def get_compute_result(self):
        return self.registers['resultSend']

    def step(self):
        self.compute()

class NoC:
    def __init__(self, rows, cols, cache_depth):
        self.rows = rows
        self.cols = cols
        self.grid = [[ProcessingElement(row, col) for col in range(cols)] for row in range(rows)]
        self.top_cache = Cache(cols, cache_depth)
        self.top_cache2 = Cache(cols, 6)
        self.bottom_cache = Cache(cols, 5)
        self.cycle = 0
        self.phase = 1
    
    def step(self):
        print(f"Cycle {self.cycle}, Phase {self.phase}")
        
        if self.phase == 1:  # Preload stage
            for row in range(self.rows - 1, -1, -1):
                for col in range(self.cols):
                    if row == 0:
                        if self.top_cache.send_register(col) != None:
                            self.grid[row][col].receive_src1(self.top_cache.send_register(col))
                    else:
                        if self.grid[row-1][col].get_src1Send() != 0:
                            self.grid[row][col].receive_src1(self.grid[row-1][col].get_src1Send())
                    print(f"PE [{row}][{col}] src1: {self.grid[row][col].get_src1()}")
                    self.grid[row][col].send_src1()
            
            if self.cycle == self.rows - 1:
                self.phase = 2  # Move to compute stage
                self.top_cache2.send_data()
                print(f"Cache2 sending first row of data2 {self.top_cache2.send_registers()} to row 0")
                # for col in range(self.cols):
                #     self.grid[0][col].receive_src2(self.top_cache2.send_register(col))
            if self.cycle < self.rows:
                self.top_cache.send_data()
                print(f"Cache sending {self.top_cache.send_registers()} to row 0")
        
        elif self.phase == 2:  # Compute stage
            # for col in range(self.cols):
            #     self.grid[0][col].receive_src2(row_data[col]
            temporary = [None] * 5
            cache_index = 5
            for row in range(self.rows - 1, -1, -1):
                for col in range(self.cols):
                    pe = self.grid[row][col]
                    pe.send_compute_result()
                    if row == 0:
                        if self.cycle == self.rows:
                            self.grid[row][col].receive_src1(self.top_cache.send_register(col))
                        pe.receive_src2(self.top_cache2.send_register(col))
                    else:
                        self.grid[row][col].receive_src1(self.grid[row-1][col].get_src1Send())
                        self.grid[row][col].receive_src2(self.grid[row-1][col].get_src2Send())
                        if row > 0:
                            self.grid[row][col].receive_compute_result(self.grid[row-1][col-1].get_compute_result())
                    print(f"PE [{row}][{col}] src1: {self.grid[row][col].get_src1()}")  
                    print(f"PE [{row}][{col}] src2: {self.grid[row][col].get_src2()}")
                    print(f"PE [{row}][{col}] received result: {self.grid[row][col].get_received_result()}")
                    pe.step()
                    #print(f"PE [{row}][{col}] computing: {pe.registers['src1']} * {pe.registers['src2']} = {pe.registers['compute_result']}")
                    
                    if row == self.rows - 1 and self.cycle > self.rows * 2 - 1:
                        print("Sending result to cache")
                        temporary[col] = pe.get_compute_result()
                        print(temporary)
                    pe.send_src2()
            if self.cycle > self.rows * 2 - 1:
                print(f"Cache receiving {temporary}")
                if cache_index >= 0:
                    self.bottom_cache.receive_data(cache_index, temporary)
                    cache_index -= 1
                else:
                    print("Cache is full")     
            if self.cycle < self.rows * 3 - 1:
                self.top_cache2.send_data()
                print(f"Cache sending {self.top_cache2.send_registers()} to row 0")
        
        self.cycle += 1
    
    def load_top_cache(self, data1):
        for i, row in enumerate(data1):
            self.top_cache.receive_data(i, row)

    def load_top_cache2(self, data1):
        for i, row in enumerate(data1):
            self.top_cache2.receive_data(i, row)
    
    def run(self, steps):
        for _ in range(steps):
            self.step()
    
    def display(self):
        for row in range(self.rows):
            row_data = []
            for col in range(self.cols):
                row_data.append(self.grid[row][col].registers['compute_result'])
            print(row_data)
        print("Bottom Cache:", self.bottom_cache.data)

# Example Usage
noc = NoC(3, 5, 3)
noc.load_top_cache([
    [0, 12, 13, 14, 15],
    [6, 7, 8, 9, 10],
    [1, 2, 3, 4, 0]
])
noc.load_top_cache2([
    [1, 2, 3, 4, 0],
    [6, 7, 8, 9, 10],
    [0, 12, 13, 14, 15],
    [0, 0, 0, 0, 0],
    [0, 0, 0, 0, 0],
    [0, 0, 0, 0, 0]
])
noc.run(steps=11)
noc.display()